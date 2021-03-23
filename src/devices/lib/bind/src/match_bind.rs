// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::bind_library;
use crate::bind_program_v2_constants::*;
use crate::bytecode_common::*;
use crate::compiler::Symbol;
use crate::decoded_bind_program::DecodedProgram;
use core::hash::Hash;
use num_traits::FromPrimitive;
use std::collections::HashMap;

#[derive(PartialEq)]
enum Condition {
    Equal,
    Inequal,
}

// TODO(fxb/71834): Currently, the driver manager only supports number-based
// device properties. It will support string-based properties soon. We should
// support other device property types in the future.
#[derive(Clone, Hash, Eq, PartialEq)]
pub enum PropertyKey {
    NumberKey(u64),
    StringKey(String),
}

type DeviceProperties = HashMap<PropertyKey, Symbol>;

struct DeviceMatcher {
    properties: DeviceProperties,
    symbol_table: HashMap<u32, String>,
    iter: BytecodeIter,
}

impl DeviceMatcher {
    pub fn new(bind_rules: DecodedProgram, properties: DeviceProperties) -> DeviceMatcher {
        DeviceMatcher {
            properties: properties,
            symbol_table: bind_rules.symbol_table,
            iter: bind_rules.instructions.into_iter(),
        }
    }

    pub fn match_bind(mut self) -> Result<bool, BytecodeError> {
        // TODO(fxb/69608): Handle jump instructions.
        while let Some(byte) = self.iter.next() {
            let op_byte = FromPrimitive::from_u8(byte).ok_or(BytecodeError::InvalidOp(byte))?;
            match op_byte {
                RawOp::EqualCondition | RawOp::InequalCondition => {
                    if !self.evaluate_condition_inst(op_byte)? {
                        return Ok(false);
                    }
                }
                RawOp::Abort => {
                    return Ok(false);
                }
                _ => {}
            };
        }

        Ok(true)
    }

    // Evaluates a conditional instruction and returns false if the condition failed.
    fn evaluate_condition_inst(&mut self, op: RawOp) -> Result<bool, BytecodeError> {
        let condition = match op {
            RawOp::EqualCondition => Condition::Equal,
            RawOp::InequalCondition => Condition::Inequal,
            _ => panic!(
                "evaluate_condition_inst() should only be called for Equal or Inequal instructions"
            ),
        };

        Ok(self.read_and_evaluate_values(condition)?)
    }

    // Read in two values and evaluate them based on the given condition.
    fn read_and_evaluate_values(&mut self, condition: Condition) -> Result<bool, BytecodeError> {
        let property_key = match self.read_next_value()? {
            Symbol::NumberValue(key) => PropertyKey::NumberKey(key),
            Symbol::StringValue(key) => PropertyKey::StringKey(key),
            Symbol::Key(key, _) => PropertyKey::StringKey(key),
            _ => unimplemented!(
                "Only number and string-based property keys are supported. See fxb/71834."
            ),
        };

        let bind_value = self.read_next_value()?;
        match self.properties.get(&property_key) {
            None => Ok(condition == Condition::Inequal),
            Some(device_value) => compare_symbols(condition, device_value, &bind_value),
        }
    }

    // Read in the next u8 as the value type and the next u32 as the value. Convert the value
    // into a Symbol.
    fn read_next_value(&mut self) -> Result<Symbol, BytecodeError> {
        let value_type = next_u8(&mut self.iter)?;
        let value_type = FromPrimitive::from_u8(value_type)
            .ok_or(BytecodeError::InvalidValueType(value_type))?;

        let value = next_u32(&mut self.iter)?;
        match value_type {
            RawValueType::NumberValue => Ok(Symbol::NumberValue(value as u64)),
            RawValueType::Key => {
                // The key's value type is a placeholder. The value type doesn't matter since
                // the only the key will be used for looking up the device property.
                Ok(Symbol::Key(self.lookup_symbol_table(value)?, bind_library::ValueType::Str))
            }
            RawValueType::StringValue => Ok(Symbol::StringValue(self.lookup_symbol_table(value)?)),
            RawValueType::BoolValue => match value {
                0x0 => Ok(Symbol::BoolValue(false)),
                0x1 => Ok(Symbol::BoolValue(true)),
                _ => Err(BytecodeError::InvalidBoolValue(value)),
            },
            RawValueType::EnumValue => Ok(Symbol::EnumValue),
        }
    }

    fn lookup_symbol_table(&self, key: u32) -> Result<String, BytecodeError> {
        self.symbol_table
            .get(&key)
            .ok_or(BytecodeError::MissingEntryInSymbolTable(key))
            .map(|val| val.to_string())
    }
}

fn compare_symbols(
    condition: Condition,
    lhs: &Symbol,
    rhs: &Symbol,
) -> Result<bool, BytecodeError> {
    if std::mem::discriminant(lhs) != std::mem::discriminant(rhs) {
        return Err(BytecodeError::MismatchValueTypes);
    }

    Ok(match condition {
        Condition::Equal => lhs == rhs,
        Condition::Inequal => lhs != rhs,
    })
}

// Return true if the bind rules matches the device properties.
pub fn match_bytecode(
    bytecode: Vec<u8>,
    properties: DeviceProperties,
) -> Result<bool, BytecodeError> {
    DeviceMatcher::new(DecodedProgram::new(bytecode)?, properties).match_bind()
}

#[cfg(test)]
mod test {
    use super::*;

    struct EncodedValue {
        value_type: RawValueType,
        value: u32,
    }

    fn append_encoded_value(bytecode: &mut Vec<u8>, encoded_val: EncodedValue) {
        bytecode.push(encoded_val.value_type as u8);
        bytecode.extend_from_slice(&encoded_val.value.to_le_bytes());
    }

    fn append_uncond_abort(bytecode: &mut Vec<u8>) {
        bytecode.push(0x30);
    }

    fn append_equal_cond(
        bytecode: &mut Vec<u8>,
        property_id: EncodedValue,
        property_value: EncodedValue,
    ) {
        bytecode.push(0x01);
        append_encoded_value(bytecode, property_id);
        append_encoded_value(bytecode, property_value);
    }

    fn append_inequal_cond(
        bytecode: &mut Vec<u8>,
        property_id: EncodedValue,
        property_value: EncodedValue,
    ) {
        bytecode.push(0x02);
        append_encoded_value(bytecode, property_id);
        append_encoded_value(bytecode, property_value);
    }

    fn verify_match_result(
        expected_result: Result<bool, BytecodeError>,
        bind_rules: DecodedProgram,
        device_properties: DeviceProperties,
    ) {
        assert_eq!(expected_result, DeviceMatcher::new(bind_rules, device_properties).match_bind());
    }

    #[test]
    fn empty_instructions() {
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: HashMap::new(), instructions: vec![] },
            HashMap::new(),
        );
    }

    #[test]
    fn equal_condition_with_number_property_keys() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(1), Symbol::NumberValue(2000));
        device_properties.insert(PropertyKey::NumberKey(2), Symbol::NumberValue(500));

        // The condition statement should match the device properties.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 2000 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the device property has a different value.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 5 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the property is missing from device properties.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 3 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 5 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn equal_condition_with_string_property_keys() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(
            PropertyKey::StringKey("nightjar".to_string()),
            Symbol::StringValue("poorwill".to_string()),
        );

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "nightjar".to_string());
        symbol_table.insert(2, "poorwill".to_string());
        symbol_table.insert(3, "nighthawk".to_string());

        // The condition statement should match the string device properties.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the string device property has a different value.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::StringValue, value: 3 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the property is missing from string device properties.
        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn inequal_condition_with_number_property_keys() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(1), Symbol::NumberValue(2000));
        device_properties.insert(PropertyKey::NumberKey(2), Symbol::NumberValue(500));

        // The condition should match since the device property has a different value.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 500 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should match since the device properties doesn't contain the property.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 10 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 5 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the device properties matches the value.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 2 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 500 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn inequal_condition_with_string_property_keys() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(
            PropertyKey::StringKey("nightjar".to_string()),
            Symbol::StringValue("poorwill".to_string()),
        );

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "nightjar".to_string());
        symbol_table.insert(2, "poorwill".to_string());

        // The condition should match since the string device property has a different value.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should match since the string device properties doesn't contain the
        // property.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );

        // The condition should fail since the string device properties matches the value.
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn unconditional_abort() {
        let mut instructions: Vec<u8> = vec![];
        append_uncond_abort(&mut instructions);
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn match_with_key_values() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties
            .insert(PropertyKey::StringKey("timberdoodle".to_string()), Symbol::NumberValue(2000));

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "timberdoodle".to_string());

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::Key, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 2000 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: symbol_table.clone(), instructions: instructions },
            device_properties.clone(),
        );

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::Key, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 500 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: symbol_table, instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn match_with_bool_values() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(1), Symbol::BoolValue(true));

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::BoolValue, value: 1 },
        );
        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::BoolValue, value: 0 },
        );
        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            device_properties.clone(),
        );
    }

    #[test]
    fn missing_entry_in_symbol_table() {
        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "nightjar".to_string());
        symbol_table.insert(2, "poorwill".to_string());

        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 10 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        verify_match_result(
            Err(BytecodeError::MissingEntryInSymbolTable(10)),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );

        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::Key, value: 15 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        verify_match_result(
            Err(BytecodeError::MissingEntryInSymbolTable(15)),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn invalid_op() {
        let mut instructions: Vec<u8> = vec![0xFF];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 10 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
        );

        verify_match_result(
            Err(BytecodeError::InvalidOp(0xFF)),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn invalid_value_type() {
        let instructions: Vec<u8> = vec![0x01, 0x05, 0, 0, 0, 0, 0x01, 0, 0, 0, 0];
        verify_match_result(
            Err(BytecodeError::InvalidValueType(0x05)),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn invalid_bool_value() {
        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 10 },
            EncodedValue { value_type: RawValueType::BoolValue, value: 15 },
        );

        verify_match_result(
            Err(BytecodeError::InvalidBoolValue(15)),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn mismatch_value_types() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(1), Symbol::NumberValue(2000));
        device_properties.insert(
            PropertyKey::StringKey("tyrant".to_string()),
            Symbol::StringValue("flycatcher".to_string()),
        );

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "tyrant".to_string());
        symbol_table.insert(2, "flycatcher".to_string());

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 15 },
        );

        verify_match_result(
            Err(BytecodeError::MismatchValueTypes),
            DecodedProgram { symbol_table: symbol_table, instructions: instructions },
            device_properties,
        );
    }

    #[test]
    fn invalid_condition_statement() {
        let instructions: Vec<u8> = vec![0x01, 0x02, 0, 0, 0];
        verify_match_result(
            Err(BytecodeError::UnexpectedEnd),
            DecodedProgram { symbol_table: HashMap::new(), instructions: instructions },
            HashMap::new(),
        );
    }

    #[test]
    fn match_with_multiple_condition_statements() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(10), Symbol::NumberValue(2000));
        device_properties.insert(PropertyKey::NumberKey(2), Symbol::NumberValue(500));
        device_properties.insert(
            PropertyKey::StringKey("rail".to_string()),
            Symbol::StringValue("crake".to_string()),
        );

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "crake".to_string());
        symbol_table.insert(2, "rail".to_string());

        let mut instructions: Vec<u8> = vec![];
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 10 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 200 },
        );
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
        );
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 10 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 2000 },
        );

        verify_match_result(
            Ok(true),
            DecodedProgram { symbol_table: symbol_table, instructions: instructions },
            device_properties,
        );
    }

    #[test]
    fn no_match_with_multiple_condition_statements() {
        let mut device_properties: DeviceProperties = HashMap::new();
        device_properties.insert(PropertyKey::NumberKey(10), Symbol::NumberValue(2000));
        device_properties.insert(PropertyKey::NumberKey(2), Symbol::NumberValue(500));
        device_properties.insert(
            PropertyKey::StringKey("rail".to_string()),
            Symbol::StringValue("crake".to_string()),
        );

        let mut symbol_table: HashMap<u32, String> = HashMap::new();
        symbol_table.insert(1, "crake".to_string());
        symbol_table.insert(2, "rail".to_string());

        let mut instructions: Vec<u8> = vec![];
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::StringValue, value: 2 },
            EncodedValue { value_type: RawValueType::StringValue, value: 1 },
        );
        append_equal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 2 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 5000 },
        );
        append_inequal_cond(
            &mut instructions,
            EncodedValue { value_type: RawValueType::NumberValue, value: 1 },
            EncodedValue { value_type: RawValueType::NumberValue, value: 40 },
        );

        verify_match_result(
            Ok(false),
            DecodedProgram { symbol_table: symbol_table, instructions: instructions },
            device_properties,
        );
    }
}