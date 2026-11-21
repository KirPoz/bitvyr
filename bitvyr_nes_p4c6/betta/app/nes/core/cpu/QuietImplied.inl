/**
 * @file QuietImplied.inl
 * @brief Общие эффекты implied-инструкций уже допущенного quiet-пути.
 * @ingroup nes_cpu
 */
// Общие эффекты quiet-регистров; включается внутри Cpu6502 с локальными и каноническими ALU-helper.
        switch (NES_CPU_QUIET_IMPLIED_OP) {
        case Op::CLC: aluSetFlag(local, C,false); break; case Op::SEC: aluSetFlag(local, C,true); break;
        case Op::CLI: aluSetFlag(local, I,false); break; case Op::SEI: aluSetFlag(local, I,true); break;
        case Op::CLD: aluSetFlag(local, D,false); break; case Op::SED: aluSetFlag(local, D,true); break;
        case Op::CLV: aluSetFlag(local, V,false); break;
        case Op::DEY: --local.y; aluSetNz(local, local.y); break; case Op::INY: ++local.y; aluSetNz(local, local.y); break;
        case Op::DEX: --local.x; aluSetNz(local, local.x); break; case Op::INX: ++local.x; aluSetNz(local, local.x); break;
        case Op::TXA: local.a=local.x; aluSetNz(local, local.a); break; case Op::TYA: local.a=local.y; aluSetNz(local, local.a); break;
        case Op::TAX: local.x=local.a; aluSetNz(local, local.x); break; case Op::TAY: local.y=local.a; aluSetNz(local, local.y); break;
        case Op::TSX: local.x=local.sp; aluSetNz(local, local.x); break; case Op::TXS: local.sp=local.x; break;
        default: break;
        }
