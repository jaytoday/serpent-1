#include <stdio.h>
#include <iostream>
#include <vector>
#include <map>
#include "util.h"
#include "bignum.h"
#include "opcodes.h"

struct programAux {
    std::map<std::string, std::string> vars;
    bool allocUsed;
    bool calldataUsed;
    int step;
    int labelLength;
};

struct programData {
    programAux aux;
    Node code;
};

programAux Aux() {
    programAux o;
    o.allocUsed = false;
    o.calldataUsed = false;
    o.step = 0;
    return o;
}

programData pd(programAux aux = Aux(), Node code=token("_")) {
    programData o;
    o.aux = aux;
    o.code = code;
    return o;
}

Node multiToken(Node nodes[], int len, Metadata met) {
    std::vector<Node> out;
    for (int i = 0; i < len; i++) {
        out.push_back(nodes[i]);
    }
    return astnode("_", out, met);
}

// Turns LLL tree into tree of code fragments
programData opcodeify(Node node, programAux aux=Aux()) {
    std::string symb = "_"+mkUniqueToken();
    Metadata m = node.metadata;
    // Numbers
    if (node.type == TOKEN) {
        return pd(aux, nodeToNumeric(node));
    }
    else if (node.val == "ref" || node.val == "get" || node.val == "set") {
        std::string varname = node.args[0].val;
        if (!aux.vars.count(varname)) {
            aux.vars[varname] = intToDecimal(aux.vars.size() * 32);
        }
        if (varname == "msg.data") aux.calldataUsed = true;
        // Set variable
        if (node.val == "set") {
             programData sub = opcodeify(node.args[1], aux);
             Node nodelist[] = {
                 token(aux.vars[varname], m),
                 sub.code,
                 token("MSTORE", m),
             };
             return pd(sub.aux, multiToken(nodelist, 3, m));                   
        }
        // Get variable
        else if (node.val == "get") {
             Node nodelist[] = 
                  { token(aux.vars[varname], m), token("MLOAD", m) };
             return pd(aux, multiToken(nodelist, 2, m));
        }
        // Refer variable
        else return pd(aux, token(aux.vars[varname], m));
    }
    std::vector<Node> subs;
    for (int i = 0; i < node.args.size(); i++) {
        programData sub = opcodeify(node.args[i], aux);
        aux = sub.aux;
        subs.push_back(sub.code);
    }
    // Seq of multiple statements
    if (node.val == "seq") {
        return pd(aux, astnode("_", subs, m));
    }
    // 2-part conditional (if gets rewritten to unless in rewrites)
    else if (node.val == "unless" && node.args.size() == 2) {
        Node nodelist[] = {
            subs[0],
            token("$endif"+symb, m), token("JUMPI", m),
            subs[1],
            token("~endif"+symb, m)
        };
        return pd(aux, multiToken(nodelist, 5, m));
    }
    // 3-part conditional
    else if (node.val == "if" && node.args.size() == 3) {
        Node nodelist[] = {
            subs[0],
            token("NOT", m), token("$else"+symb, m), token("JUMPI", m),
            subs[1],
            token("$endif"+symb, m), token("JUMP", m), token("~else"+symb, m),
            subs[2],
            token("~endif"+symb, m)
        };
        return pd(aux, multiToken(nodelist, 10, m));
    }
    // While (rewritten to this in rewrites)
    else if (node.val == "until") {
        Node nodelist[] = {
            token("~beg"+symb, m),
            subs[0],
            token("$end"+symb, m), token("JUMPI", m),
            subs[1],
            token("$beg"+symb, m), token("JUMP", m), token("~end"+symb, m)
        };
        return pd(aux, multiToken(nodelist, 8, m));
    }
    // Code blocks
    else if (node.val == "lll") {
        std::vector<Node> o;
        o.push_back(subs[0]);
        Node code = astnode("____CODE", o, m);
        Node nodelist[] = {
            token("$begincode"+symb+".endcode"+symb, m), token("DUP", m),
            subs[1],
            token("$begincode"+symb, m), token("CODECOPY", m),
            token("$endcode"+symb, m), token("JUMP", m),
            token("~begincode"+symb, m), code, token("~endcode"+symb, m)
        };
        return pd(aux, multiToken(nodelist, 10, m));
    }
    // Memory allocations
    else if (node.val == "alloc") {
        aux.allocUsed = true;
        Node nodelist[] = {
            subs[0],
            token("MSIZE", m), token("SWAP", m), token("MSIZE", m),
            token("ADD", m), token("1", m), token("SWAP", m), token("SUB", m),
            token("0", m), token("SWAP", m), token("MSTORE8", m)
        };
        return pd(aux, multiToken(nodelist, 11, m));
    }
    // Array literals
    else if (node.val == "array_lit") {
        aux.allocUsed = true;
        std::vector<Node> nodes;
        nodes.push_back(token("MSIZE", m));
        if (subs.size()) {
            nodes.push_back(token("DUP", m));
            for (int i = 0; i < subs.size(); i++) {
                nodes.push_back(subs[i]);
                nodes.push_back(token("SWAP", m));
                nodes.push_back(token("MSTORE", m));
                nodes.push_back(token("DUP", m));
                nodes.push_back(token("32", m));
                nodes.push_back(token("ADD", m));
            }
            nodes.pop_back();
            nodes.pop_back();
            nodes.pop_back();
        }
        return pd(aux, astnode("_", nodes, m));
    }
    // All other functions/operators
    else {
        subs.push_back(token(node.val, m));
        return pd(aux, astnode("_", subs, m));
    }
}

Node finalize(programData c) {
    std::vector<Node> bottom;
    Metadata m = c.code.metadata;
    // If we are using both alloc and variables, we need to pre-zfill
    // some memory
    if (c.aux.allocUsed && c.aux.vars.size() > 0) {
        Node nodelist[] = {
            token("0", m), 
            token(intToDecimal(c.aux.vars.size() * 32 - 1)),
            token("MSTORE8", m)
        };
        bottom.push_back(multiToken(nodelist, 3, m));
    }
    // If msg.data is being used as an array, then we need to copy it
    if (c.aux.calldataUsed) {
        Node nodelist[] = {
            token("MSIZE", m), token("CALLDATASIZE", m), token("MSIZE", m),
            token("0", m), token("CALLDATACOPY", m),
            token(c.aux.vars["msg.data"], m), token("MSTORE", m)
        };
        bottom.push_back(multiToken(nodelist, 7, m));
    }
    // The actual code
    bottom.push_back(c.code);
    return astnode("_", bottom, m);
}

//LLL -> code fragment tree
Node compile_lll(Node node) {
    return finalize(opcodeify(node));
}


// Builds a dictionary mapping labels to variable names
programAux buildDict(Node program, programAux aux, int labelLength) {
    Metadata m = program.metadata;
    // Token
    if (program.type == TOKEN) {
        if (isNumberLike(program)) {
            aux.step += 1 + toByteArr(program.val, m).size();
        }
        else if (program.val[0] == '~') {
            aux.vars[program.val.substr(1)] = intToDecimal(aux.step);
        }
        else if (program.val[0] == '$') {
            aux.step += labelLength + 1;
        }
        else aux.step += 1;
    }
    // A sub-program (ie. LLL)
    else if (program.val == "____CODE") {
        programAux auks = Aux();
        for (int i = 0; i < program.args.size(); i++) {
            auks = buildDict(program.args[i], auks, labelLength);
        }
        for (std::map<std::string,std::string>::iterator it=auks.vars.begin();
             it != auks.vars.end();
             it++) {
            aux.vars[(*it).first] = (*it).second;
        }
        aux.step += auks.step;
    }
    // Normal sub-block
    else {
        for (int i = 0; i < program.args.size(); i++) {
            aux = buildDict(program.args[i], aux, labelLength);
        }
    }
    return aux;
}

// Applies that dictionary
Node substDict(Node program, programAux aux, int labelLength) {
    Metadata m = program.metadata;
    std::vector<Node> out;
    std::vector<Node> inner;
    if (program.type == TOKEN) {
        if (program.val[0] == '$') {
            std::string tokStr = "PUSH"+intToDecimal(labelLength);
            out.push_back(token(tokStr, m));
            int dotLoc = program.val.find('.');
            if (dotLoc == -1) {
                inner = toByteArr(aux.vars[program.val.substr(1)], m);
            }
            else {
                std::string start = aux.vars[program.val.substr(1, dotLoc-1)],
                            end = aux.vars[program.val.substr(dotLoc + 1)],
                            dist = decimalSub(end, start);
                inner = toByteArr(dist, m);
            }
            out.push_back(astnode("_", inner, m));
        }
        else if (program.val[0] == '~') { }
        else if (isNumberLike(program)) {
            inner = toByteArr(program.val, m);
            out.push_back(token("PUSH"+intToDecimal(inner.size())));
            out.push_back(astnode("_", inner, m));
        }
        else return program;
    }
    else {
        for (int i = 0; i < program.args.size(); i++) {
            Node n = substDict(program.args[i], aux, labelLength);
            if (n.type == TOKEN || n.args.size()) out.push_back(n);
        }
    }
    return astnode("_", out, m);
}

// Compiled code -> compiled code without labels
Node dereference(Node program) {
    int sz = treeSize(program) * 4;
    int labelLength = 1;
    while (sz >= 256) labelLength += 1;
    programAux aux = buildDict(program, Aux(), labelLength);
    return substDict(program, aux, labelLength);
}

// Code fragment tree -> list
std::vector<Node> flatten(Node derefed) {
    std::vector<Node> o;
    if (derefed.type == TOKEN) {
        o.push_back(derefed);
    }
    else {
        for (int i = 0; i < derefed.args.size(); i++) {
            std::vector<Node> oprime = flatten(derefed.args[i]);
            for (int j = 0; j < oprime.size(); j++) o.push_back(oprime[j]);
        }
    }
    return o;
}

// List -> hex
std::string serialize(std::vector<Node> codons) {
    std::string o;
    for (int i = 0; i < codons.size(); i++) {
        int v;
        if (isNumberLike(codons[i])) {
            v = decimalToInt(codons[i].val);
        }
        else if (codons[i].val.substr(0,4) == "PUSH") {
            v = 95 + decimalToInt(codons[i].val.substr(4));
        }
        else {
            v = opcode(codons[i].val);
        }
        o += std::string("0123456789abcdef").substr(v/16, 1)
           + std::string("0123456789abcdef").substr(v%16, 1);
    }
    return o;
}

std::string assemble(Node program) {
    return serialize(flatten(dereference(program)));
}