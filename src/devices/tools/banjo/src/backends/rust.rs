// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::ast::{self, BanjoAst, Constant},
    crate::backends::Backend,
    crate::backends::util::to_c_name,
    failure::{format_err, Error},
    std::io,
};

type DeclIter<'a> = std::slice::Iter<'a, ast::Decl>;

pub struct RustBackend<'a, W: io::Write> {
    w: &'a mut W,
}

impl<'a, W: io::Write> RustBackend<'a, W> {
    pub fn new(w: &'a mut W) -> Self {
        RustBackend { w }
    }
}

fn to_rust_type(ast: &ast::BanjoAst, ty: &ast::Ty) -> Result<String, Error> {
    match ty {
        ast::Ty::Bool => Ok(String::from("bool")),
        ast::Ty::Int8 => Ok(String::from("i8")),
        ast::Ty::Int16 => Ok(String::from("i16")),
        ast::Ty::Int32 => Ok(String::from("i32")),
        ast::Ty::Int64 => Ok(String::from("i64")),
        ast::Ty::UInt8 => Ok(String::from("u8")),
        ast::Ty::UInt16 => Ok(String::from("u16")),
        ast::Ty::UInt32 => Ok(String::from("u32")),
        ast::Ty::UInt64 => Ok(String::from("u64")),
        ast::Ty::Float32 => Ok(String::from("f32")),
        ast::Ty::Float64 => Ok(String::from("f64")),
        ast::Ty::USize => Ok(String::from("usize")),
        ast::Ty::Array { ty, size } => {
            let Constant(ref size) = size;
            Ok(format!("[{ty}; {size} as usize]", ty = to_rust_type(&ast, ty)?, size = size.as_str().to_uppercase()))
        },
        ast::Ty::Voidptr => Ok(String::from("*mut std::ffi::c_void /* Voidptr */ ")),
        ast::Ty::Enum { .. } => Ok(String::from("*mut std::ffi::c_void /* Enum not right*/")),
        ast::Ty::Str { size, .. } => {
            match size {
                Some(Constant(c)) => {
                    Ok(format!("[u8; {size} as usize]", size = c.to_uppercase()))
                },
                None => Ok(String::from("*mut std::ffi::c_void /* String */"))
            }
        },
        ast::Ty::Vector { ref ty, size: _, nullable: _ } => to_rust_type(ast, ty),
        ast::Ty::Identifier { id, reference } => {
            if id.is_base_type() {
                Ok(format!("zircon::sys::zx_{}_t", id.name()))
            } else {
                match ast.id_to_type(id) {
                    ast::Ty::Enum => return Ok(format!("{}", name = id.name())),
                    ast::Ty::Protocol => return Ok(to_c_name(id.name())),
                    ast::Ty::Struct => {
                        let name = id.name();
                        if *reference {
                            Ok(format!("*mut {name}", name = name))
                        } else {
                            Ok(format!("{name}", name = name))
                        }
                    }
                    ast::Ty::Union => {
                        let name = id.name();
                        if *reference {
                            Ok(format!("*mut {name}", name = name))
                        } else {
                            Ok(format!("{name}", name = name))
                        }
                    }
                    t => to_rust_type(ast, &t),
                }
            }
        }
        ast::Ty::Handle { .. } => Ok(String::from("zircon::sys::zx_handle_t")),
        t => Err(format_err!("unknown type in to_rust_type {:?}", t)),
    }
}

impl<'a, W: io::Write> RustBackend<'a, W> {
    // These aren't enums, although conceptually similiar, they get generated as pub const
    // since banjo might have same value output
    fn codegen_enum_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = String::new();
        for decl in namespace {
            if let ast::Decl::Enum { ref name, ref ty, attributes: _, ref variants } = *decl {
                let mut enum_defines = Vec::new();
                let ty = to_rust_type(ast, ty)?;
                for v in variants {
                    let c_name = v.name.as_str().to_uppercase();
                    let name = if c_name.chars().next().unwrap().is_numeric() {
                        "_".to_string() + c_name.as_str()
                    } else {
                        c_name
                    };
                    enum_defines.push(format!(
                        "    pub const {name}: Self = Self({val});",
                        name = name,
                        val = v.value,
                    ));
                }

                accum.push_str(
                    format!(
                        include_str!("templates/rust/enum.rs"),
                        ty =ty,
                        name = name.name(),
                        enum_decls = enum_defines.join("\n")
                    )
                    .as_str(),
                );
            }
        }
        Ok(accum)
    }

    fn codegen_const_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = Vec::new();
        for decl in namespace {
            if let ast::Decl::Constant { ref name, ref ty, ref value, attributes: _ } = *decl {
                let Constant(ref size) = value;
                accum.push(format!(
                    "pub const {name}: {ty} = {val};",
                    name = name.name().to_uppercase(),
                    ty = to_rust_type(ast, ty)?,
                    val = size,
                ));
            }
        }
        Ok(accum.join("\n"))
    }

    fn codegen_struct_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = Vec::new();
        for decl in namespace {
            if let ast::Decl::Struct { ref name, ref fields, ref attributes } = *decl {
                let mut field_str = Vec::new();
                let alignment = if attributes.0.contains(&ast::Attr{key : "Packed".to_string(), val: None}) {
                    "packed"
                } else {
                    "C"
                };
                for field in fields {
                    field_str.push(format!(
                        "    pub {c_name}: {ty},",
                        c_name = field.ident.name(),
                        ty = to_rust_type(ast, &field.ty)?
                    ));
                }
                accum.push(format!(
                    include_str!("templates/rust/struct.rs"),
                    name = name.name(),
                    struct_fields = field_str.join("\n"),
                    alignment = alignment,
                ));
            }
        }
        Ok(accum.join("\n"))
    }

    fn codegen_union_decl(&self, namespace: DeclIter, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = Vec::new();
        for decl in namespace {
            if let ast::Decl::Union { ref name, ref fields, ref attributes } = *decl {
                let mut field_str = Vec::new();
                let alignment = if attributes.0.contains(&ast::Attr{key : "Packed".to_string(), val: None}) {
                    "packed"
                } else {
                    "C"
                };
                for field in fields {
                    field_str.push(format!(
                        "    pub {c_name}: {ty},",
                        c_name = to_c_name(field.ident.name()).as_str(),
                        ty = to_rust_type(ast, &field.ty)?
                    ));
                }
                accum.push(format!(
                    include_str!("templates/rust/union.rs"),
                    name = name.name(),
                    union_fields = field_str.join("\n"),
                    alignment = alignment,
                ));
            }
        }
        Ok(accum.join("\n"))
    }

    fn codegen_includes(&self, ast: &BanjoAst) -> Result<String, Error> {
        let mut accum = String::new();
        for n in
            ast.namespaces.iter().filter(|n| *n.0 != "zx").filter(|n| *n.0 != ast.primary_namespace)
        {
            accum.push_str(
                format!(
                    "use banjo_{name} as {name};\nuse {name}::*;\n",
                    name = n.0.replace(".", "_")
                )
                .as_str(),
            );
        }
        Ok(accum)
    }
}

impl<'a, W: io::Write> Backend<'a, W> for RustBackend<'a, W> {
    fn codegen(&mut self, ast: BanjoAst) -> Result<(), Error> {
        self.w.write_fmt(format_args!(
            include_str!("templates/rust/header.rs"),
            includes = self.codegen_includes(&ast)?,
            primary_namespace = ast.primary_namespace
        ))?;
        let namespace = &ast.namespaces[&ast.primary_namespace];
        self.w.write_fmt(format_args!(
            include_str!("templates/rust/body.rs"),
            enum_decls = self.codegen_enum_decl(namespace.iter(), &ast)?,
            constant_decls = self.codegen_const_decl(namespace.iter(), &ast)?,
            struct_decls = self.codegen_struct_decl(namespace.iter(), &ast)?,
            union_decls = self.codegen_union_decl(namespace.iter(), &ast)?,
        ))?;
        Ok(())
    }
}
