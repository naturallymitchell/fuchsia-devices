// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::compiler::BindProgram;
use crate::instruction::{Condition, Instruction, InstructionInfo};
use bitfield::bitfield;
use num_derive::FromPrimitive;
use std::fmt;

/// Functions for encoding the old bytecode format. These functions need to be
/// deleted once the old bytecode is fully deprecated.

/// These should match the values in <ddk/binding.h>, e.g. OP_ABORT = 0
#[derive(FromPrimitive, PartialEq, Clone, Copy)]
pub enum RawOp {
    Abort = 0,
    Match,
    Goto,
    Label = 5,
}

/// These should match the values in <ddk/binding.h>, e.g. COND_AL = 0
#[derive(FromPrimitive, PartialEq, Clone, Copy, Debug)]
pub enum RawCondition {
    Always = 0,
    Equal,
    NotEqual,
}

bitfield! {
    /// Each instruction is three 32 bit unsigned integers divided as follows.
    /// lsb                    msb
    /// COAABBBB VVVVVVVV DDDDDDDD Condition Opcode paramA paramB Value Debug
    ///
    /// The Debug field contains the following information:
    ///  - line: The source code line which the instruction was compiled from.
    ///  - ast_location: Where in the AST the instruction was compiled from, encoded by the
    ///      RawAstLocation enum.
    ///  - extra: Additional debugging information, meaning depends on the value of ast_location.
    ///      If ast_location is AcceptStatementFailure, extra is the key of the accept statement.
    ///      Otherwise, extra is unused.
    pub struct RawInstruction([u32]);
    u32;
    pub condition, set_condition: 31, 28;
    pub operation, set_operation: 27, 24;
    pub parameter_a, set_parameter_a: 23, 16;
    pub parameter_b, set_parameter_b: 15, 0;
    pub value, set_value: 63, 32;
    pub line, set_line: 95, 88;
    pub ast_location, set_ast_location: 87, 80;
    pub extra, set_extra: 79, 64;
}

impl fmt::Display for RawInstruction<[u32; 3]> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "c: {}, o: {}, a: {}, b: {:#06x}, v: {:#010x}",
            self.condition(),
            self.operation(),
            self.parameter_a(),
            self.parameter_b(),
            self.value()
        )
    }
}

impl From<Instruction> for RawInstruction<[u32; 3]> {
    fn from(instruction: Instruction) -> Self {
        let (c, o, a, b, v) = match instruction {
            Instruction::Abort(condition) => {
                let (c, b, v) = encode_condition(condition);
                (c, RawOp::Abort as u32, 0, b, v)
            }
            Instruction::Match(condition) => {
                let (c, b, v) = encode_condition(condition);
                (c, RawOp::Match as u32, 0, b, v)
            }
            Instruction::Goto(condition, a) => {
                let (c, b, v) = encode_condition(condition);
                (c, RawOp::Goto as u32, a, b, v)
            }
            Instruction::Label(a) => (RawCondition::Always as u32, RawOp::Label as u32, a, 0, 0),
        };

        let mut raw_instruction = RawInstruction([0, 0, 0]);
        raw_instruction.set_condition(c);
        raw_instruction.set_operation(o);
        raw_instruction.set_parameter_a(a);
        raw_instruction.set_parameter_b(b);
        raw_instruction.set_value(v);
        raw_instruction
    }
}

fn encode_condition(condition: Condition) -> (u32, u32, u32) {
    match condition {
        Condition::Always => (RawCondition::Always as u32, 0, 0),
        Condition::Equal(b, v) => (RawCondition::Equal as u32, b, v),
        Condition::NotEqual(b, v) => (RawCondition::NotEqual as u32, b, v),
    }
}

pub fn encode_instruction(info: InstructionInfo) -> RawInstruction<[u32; 3]> {
    let mut raw_instruction = RawInstruction::from(info.instruction);
    raw_instruction.set_line(info.debug.line);
    raw_instruction.set_ast_location(info.debug.ast_location as u32);
    raw_instruction.set_extra(info.debug.extra);
    raw_instruction
}

pub fn encode_to_bytecode_v1(bind_program: BindProgram) -> Vec<u8> {
    bind_program
        .instructions
        .into_iter()
        .map(|inst| encode_instruction(inst.to_instruction()))
        .flat_map(|RawInstruction([a, b, c])| {
            [a.to_le_bytes(), b.to_le_bytes(), c.to_le_bytes()].concat()
        })
        .collect::<Vec<_>>()
}

pub fn encode_to_string_v1(bind_program: BindProgram) -> String {
    bind_program
        .instructions
        .into_iter()
        .map(|inst| encode_instruction(inst.to_instruction()))
        .map(|RawInstruction([word0, word1, word2])| {
            format!("{{{:#x},{:#x},{:#x}}},", word0, word1, word2)
        })
        .collect::<String>()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::encode_bind_program_v1::RawInstruction;

    #[test]
    fn test_raw_instruction() {
        let mut raw = RawInstruction([0, 0]);
        assert_eq!(raw.0[0], 0);
        assert_eq!(raw.0[1], 0);
        assert_eq!(raw.condition(), 0);
        assert_eq!(raw.operation(), 0);
        assert_eq!(raw.parameter_a(), 0);
        assert_eq!(raw.parameter_b(), 0);
        assert_eq!(raw.value(), 0);

        raw.set_condition(1);
        assert_eq!(raw.0[0], 1 << 28);
        assert_eq!(raw.0[1], 0);
        assert_eq!(raw.condition(), 1);
        assert_eq!(raw.operation(), 0);
        assert_eq!(raw.parameter_a(), 0);
        assert_eq!(raw.parameter_b(), 0);
        assert_eq!(raw.value(), 0);

        raw.set_operation(2);
        assert_eq!(raw.0[0], (1 << 28) | (2 << 24));
        assert_eq!(raw.0[1], 0);
        assert_eq!(raw.condition(), 1);
        assert_eq!(raw.operation(), 2);
        assert_eq!(raw.parameter_a(), 0);
        assert_eq!(raw.parameter_b(), 0);
        assert_eq!(raw.value(), 0);

        raw.set_parameter_a(3);
        assert_eq!(raw.0[0], (1 << 28) | (2 << 24) | (3 << 16));
        assert_eq!(raw.0[1], 0);
        assert_eq!(raw.condition(), 1);
        assert_eq!(raw.operation(), 2);
        assert_eq!(raw.parameter_a(), 3);
        assert_eq!(raw.parameter_b(), 0);
        assert_eq!(raw.value(), 0);

        raw.set_parameter_b(4);
        assert_eq!(raw.0[0], (1 << 28) | (2 << 24) | (3 << 16) | 4);
        assert_eq!(raw.0[1], 0);
        assert_eq!(raw.condition(), 1);
        assert_eq!(raw.operation(), 2);
        assert_eq!(raw.parameter_a(), 3);
        assert_eq!(raw.parameter_b(), 4);
        assert_eq!(raw.value(), 0);

        raw.set_value(5);
        assert_eq!(raw.0[0], (1 << 28) | (2 << 24) | (3 << 16) | 4);
        assert_eq!(raw.0[1], 5);
        assert_eq!(raw.condition(), 1);
        assert_eq!(raw.operation(), 2);
        assert_eq!(raw.parameter_a(), 3);
        assert_eq!(raw.parameter_b(), 4);
        assert_eq!(raw.value(), 5)
    }

    #[test]
    fn test_abort_value() {
        let instruction = Instruction::Abort(Condition::Always);
        let raw_instruction = RawInstruction::from(instruction);
        assert_eq!(raw_instruction.0[0], 0 << 4);
        assert_eq!(raw_instruction.0[1], 0);
        assert_eq!(raw_instruction.operation(), 0)
    }

    #[test]
    fn test_match_value() {
        let instruction = Instruction::Match(Condition::Always);
        let raw_instruction = RawInstruction::from(instruction);
        assert_eq!(raw_instruction.0[0], 1 << 24);
        assert_eq!(raw_instruction.0[1], 0);
        assert_eq!(raw_instruction.operation(), 1)
    }

    #[test]
    fn test_goto_value() {
        let instruction = Instruction::Goto(Condition::Always, 0);
        let raw_instruction = RawInstruction::from(instruction);
        assert_eq!(raw_instruction.0[0], 2 << 24);
        assert_eq!(raw_instruction.0[1], 0);
        assert_eq!(raw_instruction.operation(), 2)
    }

    #[test]
    fn test_label_value() {
        let instruction = Instruction::Label(0);
        let raw_instruction = RawInstruction::from(instruction);
        assert_eq!(raw_instruction.0[0], 5 << 24);
        assert_eq!(raw_instruction.0[1], 0);
        assert_eq!(raw_instruction.operation(), 5)
    }

    #[test]
    fn test_complicated_value() {
        let instruction = Instruction::Goto(Condition::Equal(23, 1234), 42);
        let raw_instruction = RawInstruction::from(instruction);
        assert_eq!(raw_instruction.0[0], (1 << 28) | (2 << 24) | (42 << 16) | 23);
        assert_eq!(raw_instruction.0[1], 1234);
        assert_eq!(raw_instruction.condition(), 1);
        assert_eq!(raw_instruction.operation(), 2);
        assert_eq!(raw_instruction.parameter_a(), 42);
        assert_eq!(raw_instruction.parameter_b(), 23);
        assert_eq!(raw_instruction.value(), 1234);
    }
}
