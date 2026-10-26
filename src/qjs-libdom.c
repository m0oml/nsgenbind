/* QuickJS binding generation implementation
 *
 * This file is part of nsgenbind.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2015 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2026 Trevor Cockayne
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>

#include "options.h"
#include "utils.h"
#include "nsgenbind-ast.h"
#include "webidl-ast.h"
#include "ir.h"
#include "output.h"
#include "duk-libdom.h"
#include "qjs-libdom.h"

/** prefix for binding body functions, shared with duk_libdom */
#define DLPFX "dukky"

/** prefix for native QuickJS functions */
#define QJPFX "qjsky"

/**
 * prefix for hidden property names
 *
 * The compatibility layer maps names with this prefix to private symbols.
 */
#define MAGICPFX "\\xFF\\xFFNETSURF_DUKTAPE_"


/**
 * check if an ir entry is an interface with a class
 */
static bool entry_has_class(struct ir_entry *entry)
{
        return (entry->type == IR_ENTRY_TYPE_INTERFACE) &&
                (!entry->u.interface.noobject);
}


static struct opctx *open_header(struct ir *ir, const char *name)
{
        char *fname;
        int fnamel;
        struct opctx *hdrc;
        int res;

        fnamel = strlen(name) + 4;
        fname = malloc(fnamel);
        snprintf(fname, fnamel, "%s.h", name);

        /* open the output header file */
        res = output_open(fname, &hdrc);
        free(fname);
        if (res != 0 ) {
                return NULL;
        }

        /* tool preface */
        output_tool_preface(hdrc);

        /* binding preface */
        output_method_cdata(hdrc,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_PREFACE);

        /* header guard */
        outputf(hdrc,
                "\n#ifndef %s_%s_h\n", QJPFX, name);
        outputf(hdrc,
                "#define %s_%s_h\n\n", QJPFX, name);

        return hdrc;
}


static int close_header(struct ir *ir, struct opctx *hdrc)
{
        /* close header guard */
        outputf(hdrc,
                "\n#endif\n");

        /* binding postface */
        output_method_cdata(hdrc,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_POSTFACE);

        output_close(hdrc);

        return 0;
}


/**
 * generate private header
 */
static int
output_private_header(struct ir *ir)
{
        int idx;
        struct opctx *privc;

        /* open header */
        privc = open_header(ir, "private");

        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *interfacee;
                struct ir_entry *inherite;
                struct genbind_node *priv_node;

                interfacee = ir->entries + idx;

                /* do not generate private structs for interfaces marked no
                 * output
                 */
                if ((interfacee->type == IR_ENTRY_TYPE_INTERFACE) &&
                    (interfacee->u.interface.noobject)) {
                        continue;
                }

                switch (interfacee->type) {
                case IR_ENTRY_TYPE_INTERFACE:
                        outputf(privc,
                                "/* Private data for %s interface */\n",
                                interfacee->name);
                        break;

                case IR_ENTRY_TYPE_DICTIONARY:
                        outputf(privc,
                                "/* Private data for %s dictionary */\n",
                                interfacee->name);
                        break;
                }

                outputf(privc,
                        "typedef struct {\n");

                /* find parent entry and include in private */
                inherite = ir_inherit_entry(ir, interfacee);
                if (inherite != NULL) {
                        outputf(privc,
                                "\t%s_private_t parent;\n",
                                inherite->class_name);
                }

                /* for each private variable on the class output it here. */
                priv_node = genbind_node_find_type(
                        genbind_node_getnode(interfacee->class),
                        NULL,
                        GENBIND_NODE_TYPE_PRIVATE);
                while (priv_node != NULL) {
                        outputc(privc, '\t');

                        output_ctype(privc, priv_node, true);

                        outputf(privc, ";\n");

                        priv_node = genbind_node_find_type(
                                genbind_node_getnode(interfacee->class),
                                priv_node,
                                GENBIND_NODE_TYPE_PRIVATE);
                }

                outputf(privc,
                        "} __attribute__((aligned)) %s_private_t;\n\n",
                        interfacee->class_name);
        }

        close_header(ir, privc);

        return 0;
}


/**
 * generate prototype header
 */
static int
output_prototype_header(struct ir *ir)
{
        int idx;
        struct opctx *protoc;

        /* open header */
        protoc = open_header(ir, "prototype");

        outputf(protoc, "struct qjsky_class;\n\n");

        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *entry;

                entry = ir->entries + idx;

                switch (entry->type) {
                case IR_ENTRY_TYPE_INTERFACE:
                        qjs_output_interface_declaration(protoc, entry);
                        break;

                case IR_ENTRY_TYPE_DICTIONARY:
                        output_dictionary_declaration(protoc, entry);
                        break;
                }
        }

        close_header(ir, protoc);

        return 0;
}


/**
 * generate makefile fragment
 */
static int
output_makefile(struct ir *ir)
{
        int idx;
        FILE *makef;

        /* open output file */
        makef = genb_fopen_tmp("Makefile");
        if (makef == NULL) {
                return -1;
        }

        fprintf(makef, "# qjs libdom makefile fragment\n\n");

        fprintf(makef, "NSGENBIND_SOURCES:=binding.c ");
        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *interfacee;

                interfacee = ir->entries + idx;

                /* no source for interfaces marked no output */
                if ((interfacee->type == IR_ENTRY_TYPE_INTERFACE) &&
                    (interfacee->u.interface.noobject)) {
                        continue;
                }

                fprintf(makef, "%s ", interfacee->filename);
        }
        fprintf(makef, "\nNSGENBIND_PREFIX:=%s\n", options->outdirname);

        genb_fclose_tmp(makef, "Makefile");

        return 0;
}


/**
 * generate binding header
 *
 * The binding header contains the hidden name macros used by binding
 * bodies, the enumeration of classes and the class table declaration.
 *
 * Classes are enumerated in the ir order which places parents before
 * the interfaces which inherit from them.
 */
static int
output_binding_header(struct ir *ir)
{
        struct opctx *bindc;
        struct ir_entry *pglobale = NULL;
        char *proto_name;
        int idx;

        /* open header */
        bindc = open_header(ir, "binding");

        outputf(bindc,
                "#define _MAGIC(S) (\"%s\" S)\n"
                "#define MAGIC(S) _MAGIC(#S)\n"
                "#define PROTO_MAGIC MAGIC(PROTOTYPES)\n"
                "#define PRIVATE_MAGIC MAGIC(PRIVATE)\n"
                "#define INIT_MAGIC MAGIC(INIT)\n"
                "#define NODE_MAGIC MAGIC(NODE_MAP)\n"
                "#define PROTO_NAME(K) QJSKY_PROTO_##K\n"
                "#define _PROP_NAME(K,V) _MAGIC(K \"_PROPERTY_\" V)\n"
                "#define PROP_NAME(K,V) _PROP_NAME(#K,#V)\n"
                "\n",
                MAGICPFX);

        /* class enumeration */
        outputf(bindc, "/* Classes in creation order */\nenum qjsky_proto {\n");
        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *entry = ir->entries + idx;

                if (!entry_has_class(entry)) {
                        continue;
                }
                if (entry->u.interface.primary_global) {
                        pglobale = entry;
                }

                proto_name = qjs_proto_name(entry->name);
                outputf(bindc, "\tQJSKY_PROTO_%s,\n", proto_name);
                free(proto_name);
        }
        outputf(bindc, "\tQJSKY_PROTO__COUNT\n};\n\n");

        if (pglobale != NULL) {
                proto_name = qjs_proto_name(pglobale->name);
                outputf(bindc,
                        "/* The primary global object class */\n"
                        "#define QJSKY_PROTO_GLOBAL QJSKY_PROTO_%s\n\n",
                        proto_name);
                free(proto_name);
        }

        /* declaration of constant string values */
        outputf(bindc,
                "/* Constant strings */\n"
                "extern const char *%s_error_fmt_argument;\n"
                "extern const char *%s_error_fmt_bool_type;\n"
                "extern const char *%s_error_fmt_number_type;\n"
                "extern const char *%s_magic_string_private;\n"
                "\n",
                DLPFX, DLPFX, DLPFX, DLPFX);

        outputf(bindc,
                "struct qjsky_class;\n"
                "\n"
                "/* Classes indexed by enum qjsky_proto */\n"
                "extern const struct qjsky_class *const %s_classes[QJSKY_PROTO__COUNT];\n",
                QJPFX);

        close_header(ir, bindc);

        return 0;
}


/**
 * generate binding source
 */
static int
output_binding_src(struct ir *ir)
{
        int idx;
        struct opctx *bindc;
        char *proto_name;
        int res;

        /* open the output binding file */
        res = output_open("binding.c", &bindc);
        if (res != 0 ) {
                return -1;
        }

        /* tool preface */
        output_tool_preface(bindc);

        /* binding preface */
        output_method_cdata(bindc,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_PREFACE);

        /* tool prologue */
        output_tool_prologue(bindc);

        /* binding prologue */
        output_method_cdata(bindc,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_PROLOGUE);

        outputc(bindc, '\n');

        outputf(bindc,
                "/* Error format strings */\n"
                "const char *%s_error_fmt_argument =\"%%d argument required, but ony %%d present.\";\n"
                "const char *%s_error_fmt_bool_type =\"argument %%d (%%s) requires a bool\";\n"
                "const char *%s_error_fmt_number_type =\"argument %%d (%%s) requires a number\";\n",
                DLPFX, DLPFX, DLPFX);

        outputf(bindc, "\n");

        outputf(bindc,
                "/* Magic identifiers */\n"
                "const char *%s_magic_string_private =\"%sPRIVATE\";\n",
                DLPFX, MAGICPFX);

        outputf(bindc, "\n");

        /* class table */
        outputf(bindc,
                "const struct qjsky_class *const %s_classes[QJSKY_PROTO__COUNT] = {\n",
                QJPFX);
        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *entry = ir->entries + idx;

                if (!entry_has_class(entry)) {
                        continue;
                }

                proto_name = qjs_proto_name(entry->name);
                outputf(bindc,
                        "\t[QJSKY_PROTO_%s] = &%s_%s___class,\n",
                        proto_name, QJPFX, entry->class_name);
                free(proto_name);
        }
        outputf(bindc, "};\n");

        /* binding postface */
        output_method_cdata(bindc,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_POSTFACE);

        output_close(bindc);

        return 0;
}


static int output_interfaces_dictionaries(struct ir *ir)
{
        int res;
        int idx;

        /* generate interfaces */
        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *irentry;

                irentry = ir->entries + idx;

                switch (irentry->type) {
                case IR_ENTRY_TYPE_INTERFACE:
                        /* do not generate class for interfaces marked no
                         * output
                         */
                        if (!irentry->u.interface.noobject) {
                                res = qjs_output_interface(ir, irentry);
                                if (res != 0) {
                                        return res;
                                }
                        }
                        break;

                case IR_ENTRY_TYPE_DICTIONARY:
                        /* dictionary accessors only use the value stack
                         * so the duk_libdom generator is shared
                         */
                        res = output_dictionary(ir, irentry);
                        if (res != 0) {
                                return res;
                        }
                        break;

                default:
                        break;
                }
        }

        return 0;
}


/* exported interface documented in qjs-libdom.h */
int qjs_libdom_output(struct ir *ir)
{
        int idx;
        int res = 0;

        /* process ir entries for output */
        for (idx = 0; idx < ir->entryc; idx++) {
                struct ir_entry *irentry;

                irentry = ir->entries + idx;

                /* compute class name */
                irentry->class_name = gen_idl2c_name(irentry->name);

                if (irentry->class_name != NULL) {
                        int ifacenamelen;

                        /* generate source filename */
                        ifacenamelen = strlen(irentry->class_name) + 4;
                        irentry->filename = malloc(ifacenamelen);
                        snprintf(irentry->filename,
                                 ifacenamelen,
                                 "%s.c",
                                 irentry->class_name);
                }
        }

        res = output_interfaces_dictionaries(ir);
        if (res != 0) {
                goto output_err;
        }

        /* generate private header */
        res = output_private_header(ir);
        if (res != 0) {
                goto output_err;
        }

        /* generate prototype header */
        res = output_prototype_header(ir);
        if (res != 0) {
                goto output_err;
        }

        /* generate binding header */
        res = output_binding_header(ir);
        if (res != 0) {
                goto output_err;
        }

        /* generate binding source */
        res = output_binding_src(ir);
        if (res != 0) {
                goto output_err;
        }

        /* generate makefile fragment */
        res = output_makefile(ir);

output_err:

        return res;
}
