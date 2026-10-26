/* QuickJS binding generation
 *
 * This file is part of nsgenbind.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 Trevor Cockayne
 */

#ifndef nsgenbind_qjs_libdom_h
#define nsgenbind_qjs_libdom_h

/**
 * Generate output for QuickJS and libdom bindings.
 *
 * The generated code uses native QuickJS classes, prototypes and
 * finalisers.  The binding bodies are written against the duktape value
 * stack API so are run through a duktape compatibility layer, which
 * allows the duk_libdom dictionary and attribute generators to be
 * shared.
 */
int qjs_libdom_output(struct ir *ir);

/**
 * generate a source file to implement an interface using QuickJS and libdom.
 *
 * \param ir The intermediate representation of the binding tree.
 * \param interfacee The interface entry to output within the ir.
 */
int qjs_output_interface(struct ir *ir, struct ir_entry *interfacee);

/**
 * generate a declaration to implement an interface using QuickJS and libdom.
 */
int qjs_output_interface_declaration(struct opctx *outc, struct ir_entry *interfacee);

/**
 * generate the prototype enumeration name of an interface.
 *
 * \param interface_name The IDL interface name
 * \return The upper case name which must be freed by the caller
 */
char *qjs_proto_name(const char *interface_name);

#endif
