#include "sim/simulator.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace sim {
namespace {

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

int parseReg(const std::string& tok) {
    if (tok.size() < 2 || (tok[0] != 'r' && tok[0] != 'R' && tok[0] != 'x' && tok[0] != 'X')) {
        throw std::runtime_error("bad register: " + tok);
    }
    const int n = std::stoi(tok.substr(1));
    if (n < 0 || n > 31) throw std::runtime_error("register out of range: " + tok);
    return n;
}

struct MemOperand {
    int imm;
    int rs;
};

MemOperand parseMem(const std::string& tok) {
    const auto lp = tok.find('(');
    const auto rp = tok.find(')');
    if (lp == std::string::npos || rp == std::string::npos) throw std::runtime_error("bad mem operand: " + tok);
    return MemOperand{std::stoi(tok.substr(0, lp)), parseReg(tok.substr(lp + 1, rp - lp - 1))};
}

std::vector<std::string> splitOperands(const std::string& rest) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : rest) {
        if (c == ',') {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

const std::unordered_map<std::string, Op>& threeReg() {
    static const std::unordered_map<std::string, Op> table = {
        {"ADD", Op::Add}, {"SUB", Op::Sub}, {"MUL", Op::Mul}, {"AND", Op::And}, {"OR", Op::Or},
        {"XOR", Op::Xor}, {"SLT", Op::Slt}, {"SLL", Op::Sll}, {"SRL", Op::Srl},
    };
    return table;
}

const std::unordered_map<std::string, Op>& regImm() {
    static const std::unordered_map<std::string, Op> table = {
        {"ADDI", Op::Addi}, {"ANDI", Op::Andi}, {"SLLI", Op::Slli},
    };
    return table;
}

const std::unordered_map<std::string, Op>& branches() {
    static const std::unordered_map<std::string, Op> table = {
        {"BEQ", Op::Beq}, {"BNE", Op::Bne}, {"BLT", Op::Blt}, {"BGE", Op::Bge},
    };
    return table;
}

}  // namespace

const char* opName(Op op) {
    switch (op) {
        case Op::Add: return "ADD";
        case Op::Addi: return "ADDI";
        case Op::Sub: return "SUB";
        case Op::Mul: return "MUL";
        case Op::And: return "AND";
        case Op::Andi: return "ANDI";
        case Op::Or: return "OR";
        case Op::Xor: return "XOR";
        case Op::Slt: return "SLT";
        case Op::Sll: return "SLL";
        case Op::Slli: return "SLLI";
        case Op::Srl: return "SRL";
        case Op::Lw: return "LW";
        case Op::Sw: return "SW";
        case Op::Beq: return "BEQ";
        case Op::Bne: return "BNE";
        case Op::Blt: return "BLT";
        case Op::Bge: return "BGE";
        case Op::J: return "J";
        case Op::Nop: return "NOP";
    }
    return "?";
}

std::vector<Instr> parseProgram(const std::string& text) {
    struct Raw {
        std::string op;
        std::vector<std::string> args;
    };
    std::vector<Raw> raws;
    std::unordered_map<std::string, int> labels;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.back() == ':') {
            labels[trim(line.substr(0, line.size() - 1))] = static_cast<int>(raws.size());
            continue;
        }
        const auto sp = line.find_first_of(" \t");
        std::string op = sp == std::string::npos ? line : line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
        for (char& c : op) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        raws.push_back(Raw{op, splitOperands(rest)});
    }

    std::vector<Instr> prog;
    prog.reserve(raws.size());
    for (int i = 0; i < static_cast<int>(raws.size()); ++i) {
        const auto& r = raws[static_cast<size_t>(i)];
        Instr ins;
        ins.pc = i;
        const auto need = [&](size_t n) {
            if (r.args.size() != n) throw std::runtime_error("bad operands for " + r.op);
        };
        const auto labelAt = [&](const std::string& name) {
            const auto it = labels.find(name);
            if (it == labels.end()) throw std::runtime_error("unknown label " + name);
            return it->second;
        };
        if (const auto it = threeReg().find(r.op); it != threeReg().end()) {
            need(3);
            ins.op = it->second;
            ins.rd = parseReg(r.args[0]);
            ins.rs = parseReg(r.args[1]);
            ins.rt = parseReg(r.args[2]);
        } else if (const auto imm = regImm().find(r.op); imm != regImm().end()) {
            need(3);
            ins.op = imm->second;
            ins.rd = parseReg(r.args[0]);
            ins.rs = parseReg(r.args[1]);
            ins.imm = std::stoi(r.args[2]);
        } else if (r.op == "LW") {
            need(2);
            ins.op = Op::Lw;
            ins.rd = parseReg(r.args[0]);
            const auto m = parseMem(r.args[1]);
            ins.imm = m.imm;
            ins.rs = m.rs;
        } else if (r.op == "SW") {
            need(2);
            ins.op = Op::Sw;
            ins.rt = parseReg(r.args[0]);
            const auto m = parseMem(r.args[1]);
            ins.imm = m.imm;
            ins.rs = m.rs;
        } else if (const auto br = branches().find(r.op); br != branches().end()) {
            need(3);
            ins.op = br->second;
            ins.rs = parseReg(r.args[0]);
            ins.rt = parseReg(r.args[1]);
            ins.imm = labelAt(r.args[2]);
        } else if (r.op == "J") {
            need(1);
            ins.op = Op::J;
            ins.imm = labelAt(r.args[0]);
        } else if (r.op == "NOP") {
            need(0);
            ins.op = Op::Nop;
        } else {
            throw std::runtime_error("unknown opcode " + r.op);
        }
        prog.push_back(ins);
    }
    return prog;
}

}  // namespace sim
