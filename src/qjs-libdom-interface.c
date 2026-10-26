/* QuickJS binding generation implementation
 *
 * This file is part of nsgenbind.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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


/* exported function documented in qjs-libdom.h */
char *qjs_proto_name(const char *interface_name)
{
        char *proto_name;
        int idx;

        proto_name = strdup(interface_name);
        for (idx = 0; proto_name[idx] != 0; idx++) {
                proto_name[idx] = toupper(proto_name[idx]);
        }
        return proto_name;
}


/**
 * Compare two nodes to check their c types match.
 */
static bool compare_ctypes(struct genbind_node *a, struct genbind_node *b)
{
        struct genbind_node *ta;
        struct genbind_node *tb;

        ta = genbind_node_find_type(genbind_node_getnode(a),
                                    NULL, GENBIND_NODE_TYPE_NAME);
        tb = genbind_node_find_type(genbind_node_getnode(b),
                                    NULL, GENBIND_NODE_TYPE_NAME);

        while ((ta != NULL) && (tb != NULL)) {
                char *txt_a;
                char *txt_b;

                txt_a = genbind_node_gettext(ta);
                txt_b = genbind_node_gettext(tb);

                if (strcmp(txt_a, txt_b) != 0) {
                        return false; /* missmatch */
                }

                ta = genbind_node_find_type(genbind_node_getnode(a),
                                            ta, GENBIND_NODE_TYPE_NAME);
                tb = genbind_node_find_type(genbind_node_getnode(b),
                                            tb, GENBIND_NODE_TYPE_NAME);
        }
        if (ta != tb) {
                return false;
        }

        return true;
}


/**
 * generate an initialisor call to parent interface
 */
static int
output_interface_inherit_init(struct opctx *outc,
                              struct ir_entry *interfacee,
                              struct ir_entry *inherite)
{
        struct genbind_node *init_node;
        struct genbind_node *inh_init_node;
        struct genbind_node *param_node;
        struct genbind_node *inh_param_node;

        /* only need to call parent initialisor if there is one */
        if (inherite == NULL) {
                return 0;
        }

        /* find the initialisor method on the class (if any) */
        init_node = genbind_node_find_method(interfacee->class,
                                             NULL,
                                             GENBIND_METHOD_TYPE_INIT);


        inh_init_node = genbind_node_find_method(inherite->class,
                                                 NULL,
                                                 GENBIND_METHOD_TYPE_INIT);


        outputf(outc,
                "\t%s_%s___init(ctx, &priv->parent",
                DLPFX, inherite->class_name);

        /* for each parameter in the parent find a matching named
         * parameter to pass and cast if necessary
         */

        inh_param_node = genbind_node_find_type(
                genbind_node_getnode(inh_init_node),
                NULL, GENBIND_NODE_TYPE_PARAMETER);
        while (inh_param_node != NULL) {
                char *param_name;
                param_name = genbind_node_gettext(
                        genbind_node_find_type(
                                genbind_node_getnode(inh_param_node),
                                NULL,
                                GENBIND_NODE_TYPE_IDENT));

                param_node = genbind_node_find_type_ident(
                        genbind_node_getnode(init_node),
                        NULL,
                        GENBIND_NODE_TYPE_PARAMETER,
                        param_name);
                if (param_node == NULL) {
                        fprintf(stderr,
                                "class \"%s\" (interface %s) parent class \"%s\" (interface %s) initialisor requires a parameter \"%s\" with compatible identifier\n",
                                interfacee->class_name,
                                interfacee->name,
                                inherite->class_name,
                                inherite->name,
                                param_name);
                        return -1;
                } else {
                        outputf(outc, ", ");

                        /* cast the parameter if required */
                        if (compare_ctypes(param_node,
                                           inh_param_node) == false) {
                                outputc(outc, '(');
                                output_ctype(outc, inh_param_node, false);
                                outputc(outc, ')');
                        }

                        /* output the parameter identifier */
                        output_cdata(outc, param_node, GENBIND_NODE_TYPE_IDENT);
                }

                inh_param_node = genbind_node_find_type(
                        genbind_node_getnode(inh_init_node),
                        inh_param_node, GENBIND_NODE_TYPE_METHOD);
        }

        outputf(outc, ");\n");

        return 0;
}


static enum ir_init_argtype
guess_argtype_from(struct genbind_node *param_node)
{
        const char *type_cdata = NULL;
        struct genbind_node *typename_node;
        bool unsigned_ = false;
        bool int_ = false;
        bool bool_ = false;

        typename_node = genbind_node_find_type(genbind_node_getnode(param_node),
                                               NULL,
                                               GENBIND_NODE_TYPE_NAME);
        while (typename_node != NULL) {
                type_cdata = genbind_node_gettext(typename_node);
                if (strcmp(type_cdata, "unsigned") == 0) {
                        unsigned_ = true;
                } else if (strcmp(type_cdata, "int") == 0) {
                        int_ = true;
                } else if (strcmp(type_cdata, "bool") == 0) {
                        bool_ = true;
                }
                typename_node = genbind_node_find_type(
                        genbind_node_getnode(param_node),
                        typename_node,
                        GENBIND_NODE_TYPE_NAME);
        }

        if (type_cdata[0] == '*') {
                return IR_INIT_ARG_POINTER;
        } else if (unsigned_) {
                return IR_INIT_ARG_UNSIGNED;
        } else if (int_) {
                return IR_INIT_ARG_INT;
        } else if (bool_) {
                return IR_INIT_ARG_BOOL;
        }

        /* If we have no better idea do this */
        return IR_INIT_ARG_POINTER;
}


static int
output_interface_init_declaration(struct opctx *outc,
                                  struct ir_entry *interfacee,
                                  struct genbind_node *init_node)
{
        struct genbind_node *param_node;

        if  (interfacee->refcount == 0) {
                outputf(outc, "static ");
        }

        outputf(outc,
                "void %s_%s___init(duk_context *ctx, %s_private_t *priv",
                DLPFX, interfacee->class_name, interfacee->class_name);

        /* count the number of arguments on the initializer */
        interfacee->class_init_argc = 0;
        free(interfacee->class_init_argt);
        interfacee->class_init_argt = NULL;

        /* output the paramters on the method (if any) */
        param_node = genbind_node_find_type(
                genbind_node_getnode(init_node),
                NULL, GENBIND_NODE_TYPE_PARAMETER);
        while (param_node != NULL) {
                interfacee->class_init_argc++;
                interfacee->class_init_argt = realloc(interfacee->class_init_argt,
                                                      interfacee->class_init_argc * sizeof(enum ir_init_argtype));
                interfacee->class_init_argt[interfacee->class_init_argc - 1] =
                        guess_argtype_from(param_node);
                outputf(outc, ", ");

                output_ctype(outc, param_node, true);

                param_node = genbind_node_find_type(
                        genbind_node_getnode(init_node),
                        param_node, GENBIND_NODE_TYPE_PARAMETER);
        }

        outputc(outc, ')');

        return 0;
}


/**
 * generate code for interface (class) initialisor
 *
 * The object being initialised is at stack index 0 and the
 * initialiser arguments follow it.
 */
static int
output_interface_init(struct opctx *outc,
                      struct ir_entry *interfacee,
                      struct ir_entry *inherite)
{
        struct genbind_node *init_node;
        int res;

        /* find the initialisor method on the class (if any) */
        init_node = genbind_node_find_method(interfacee->class,
                                             NULL,
                                             GENBIND_METHOD_TYPE_INIT);

        /* initialisor definition */
        output_interface_init_declaration(outc, interfacee, init_node);

        outputf(outc, "\n{\n");

        /* if this interface inherits ensure we call its initialisor */
        res = output_interface_inherit_init(outc, interfacee, inherite);
        if (res != 0) {
                return res;
        }

        /* generate log statement */
        if (options->dbglog) {
                outputf(outc,
                        "\tNSLOG(qjsky, DEEPDEBUG, \"Initialise %s (priv=%%p)\", priv);\n",
                        interfacee->name);
        }

        /* output the initaliser code from the binding */
        output_ccode(outc, init_node);

        outputf(outc, "}\n\n");

        return 0;
}


/**
 * generate code for interface (class) finaliser
 *
 * The context is NULL when called from the garbage collector so
 * finalisers must not use the value stack.
 */
static int
output_interface_fini(struct opctx *outc,
                      struct ir_entry *interfacee,
                      struct ir_entry *inherite)
{
        struct genbind_node *fini_node;

        /* find the finaliser method on the class (if any) */
        fini_node = genbind_node_find_method(interfacee->class,
                                             NULL,
                                             GENBIND_METHOD_TYPE_FINI);

        /* finaliser definition */
        if  (interfacee->refcount == 0) {
                outputf(outc, "static ");
        }
        outputf(outc,
                "void %s_%s___fini(duk_context *ctx, %s_private_t *priv)\n",
                DLPFX, interfacee->class_name, interfacee->class_name);
        outputf(outc, "{\n");

        /* generate log statement */
        if (options->dbglog) {
                outputf(outc,
                        "\tNSLOG(qjsky, DEEPDEBUG, \"Finalise %s (priv=%%p)\", priv);\n",
                        interfacee->name);
        }

        /* output the finialisor code from the binding */
        output_cdata(outc, fini_node, GENBIND_NODE_TYPE_CDATA);

        /* if this interface inherits ensure we call its finaliser */
        if (inherite != NULL) {
                outputf(outc,
                        "\t%s_%s___fini(ctx, &priv->parent);\n",
                        DLPFX, inherite->class_name);
        }
        outputf(outc, "}\n\n");

        return 0;
}


/**
 * generate the construct and destroy functions used by the class table
 */
static int
output_interface_construct(struct opctx *outc, struct ir_entry *interfacee)
{
        int init_argc;

        outputf(outc,
                "static void %s_%s___construct(duk_context *ctx, void *priv)\n"
                "{\n"
                "\t%s_%s___init(ctx, priv",
                QJPFX, interfacee->class_name,
                DLPFX, interfacee->class_name);

        for (init_argc = 1;
             init_argc <= interfacee->class_init_argc;
             init_argc++) {
                switch (interfacee->class_init_argt[init_argc-1]) {
                case IR_INIT_ARG_BOOL:
                        outputf(outc,
                                ", duk_get_boolean(ctx, %d)",
                                init_argc);
                        break;
                case IR_INIT_ARG_UNSIGNED:
                        outputf(outc,
                                ", duk_get_uint(ctx, %d)",
                                init_argc);
                        break;
                case IR_INIT_ARG_INT:
                        outputf(outc,
                                ", duk_get_int(ctx, %d)",
                                init_argc);
                        break;
                case IR_INIT_ARG_POINTER:
                        outputf(outc,
                                ", duk_get_pointer(ctx, %d)",
                                init_argc);
                        break;
                }
        }
        outputf(outc, ");\n}\n\n");

        outputf(outc,
                "static void %s_%s___destroy(duk_context *ctx, void *priv)\n"
                "{\n"
                "\t%s_%s___fini(ctx, priv);\n"
                "}\n\n",
                QJPFX, interfacee->class_name,
                DLPFX, interfacee->class_name);

        return 0;
}


/**
 * generate the native QuickJS function which calls a binding body
 *
 * \param outc The output context
 * \param interfacee The interface
 * \param name The name of the body and native functions within the class
 * \param kind 0 for a method, 1 for a getter, 2 for a setter
 */
static int
output_native_wrapper(struct opctx *outc,
                      struct ir_entry *interfacee,
                      const char *name,
                      int kind)
{
        char *proto_name;
        const char *argc;
        const char *argv;

        proto_name = qjs_proto_name(interfacee->name);

        switch (kind) {
        case 1:
                outputf(outc,
                        "static JSValue %s_%s_%s(JSContext *jctx, JSValueConst this_val)\n",
                        QJPFX, interfacee->class_name, name);
                argc = "0";
                argv = "NULL";
                break;

        case 2:
                outputf(outc,
                        "static JSValue %s_%s_%s(JSContext *jctx, JSValueConst this_val, JSValueConst val)\n",
                        QJPFX, interfacee->class_name, name);
                argc = "1";
                argv = "&val";
                break;

        default:
                outputf(outc,
                        "static JSValue %s_%s_%s(JSContext *jctx, JSValueConst this_val, int argc, JSValueConst *argv)\n",
                        QJPFX, interfacee->class_name, name);
                argc = "argc";
                argv = "argv";
                break;
        }

        outputf(outc,
                "{\n"
                "\tvoid *priv = %s_get_private(jctx, this_val, QJSKY_PROTO_%s);\n"
                "\tif (priv == NULL) {\n"
                "\t\treturn %s_illegal_invocation(jctx, \"%s\", \"%s\");\n"
                "\t}\n",
                QJPFX, proto_name,
                QJPFX, interfacee->name, name);

        if (interfacee->u.interface.primary_global) {
                /* the global is this whether or not the call had one */
                outputf(outc,
                        "\tJSValue global = JS_GetGlobalObject(jctx);\n"
                        "\tJSValue res = qjsduk_invoke(jctx, global, %s, %s, %s_%s_%s, priv);\n"
                        "\tJS_FreeValue(jctx, global);\n"
                        "\treturn res;\n",
                        argc, argv, DLPFX, interfacee->class_name, name);
        } else {
                outputf(outc,
                        "\treturn qjsduk_invoke(jctx, this_val, %s, %s, %s_%s_%s, priv);\n",
                        argc, argv, DLPFX, interfacee->class_name, name);
        }

        outputf(outc, "}\n\n");

        free(proto_name);

        return 0;
}


/**
 * generate the start of a binding body function
 */
static int
output_body_start(struct opctx *outc,
                  struct ir_entry *interfacee,
                  const char *name)
{
        outputf(outc,
                "static duk_ret_t %s_%s_%s(duk_context *ctx, void *upriv)\n"
                "{\n"
                "\t%s_private_t *priv = upriv;\n"
                "\t(void)priv; /* not all bodies use it */\n",
                DLPFX, interfacee->class_name, name,
                interfacee->class_name);

        return 0;
}


/**
 * generate default values on the duk stack
 */
static int
output_operation_optional_defaults(
        struct opctx *outc,
        struct ir_operation_argument_entry *argumentv,
        int argumentc)
{
        int argc;
        for (argc = 0; argc < argumentc; argc++) {
                struct ir_operation_argument_entry *cure;
                struct webidl_node *lit_node; /* literal node */
                enum webidl_node_type lit_type;
                int *lit_int;
                char *lit_str;

                cure = argumentv + argc;

                lit_node = webidl_node_getnode(
                        webidl_node_find_type(
                                webidl_node_getnode(cure->node),
                                NULL,
                                WEBIDL_NODE_TYPE_OPTIONAL));

                if (lit_node != NULL) {

                        lit_type = webidl_node_gettype(lit_node);

                        switch (lit_type) {
                        case WEBIDL_NODE_TYPE_LITERAL_NULL:
                                outputf(outc,
                                        "\t\tduk_push_null(ctx);\n");
                                break;

                        case WEBIDL_NODE_TYPE_LITERAL_INT:
                                lit_int = webidl_node_getint(lit_node);
                                outputf(outc,
                                        "\t\tduk_push_int(ctx, %d);\n",
                                        *lit_int);
                                break;

                        case WEBIDL_NODE_TYPE_LITERAL_BOOL:
                                lit_int = webidl_node_getint(lit_node);
                                outputf(outc,
                                        "\t\tduk_push_boolean(ctx, %d);\n",
                                        *lit_int);
                                break;

                        case WEBIDL_NODE_TYPE_LITERAL_STRING:
                                lit_str = webidl_node_gettext(lit_node);
                                outputf(outc,
                                        "\t\tduk_push_string(ctx, \"%s\");\n",
                                        lit_str);
                                break;

                        case WEBIDL_NODE_TYPE_LITERAL_FLOAT:
                        default:
                                outputf(outc,
                                        "\t\tduk_push_undefined(ctx);\n");
                                break;
                        }
                } else {
                        outputf(outc,
                                "\t\tduk_push_undefined(ctx);\n");
                }
        }
        return 0;
}


static int
output_operation_argument_type_check(
        struct opctx *outc,
        struct ir_entry *interfacee,
        struct ir_operation_entry *operatione,
        struct ir_operation_overload_entry *overloade,
        int argidx)
{
        struct ir_operation_argument_entry *argumente;
        struct webidl_node *type_node;
        enum webidl_type *argument_type;

        argumente = overloade->argumentv + argidx;

        type_node = webidl_node_find_type(
                webidl_node_getnode(argumente->node),
                NULL,
                WEBIDL_NODE_TYPE_TYPE);

        if (type_node == NULL) {
                fprintf(stderr, "%s:%s %dth argument %s has no type\n",
                        interfacee->name,
                        operatione->name,
                        argidx,
                        argumente->name);
                return -1;
        }

        argument_type = (enum webidl_type *)webidl_node_getint(
                webidl_node_find_type(
                        webidl_node_getnode(type_node),
                        NULL,
                        WEBIDL_NODE_TYPE_TYPE_BASE));

        if (argument_type == NULL) {
                fprintf(stderr,
                        "%s:%s %dth argument %s has no type base\n",
                        interfacee->name,
                        operatione->name,
                        argidx,
                        argumente->name);
                return -1;
        }

        if (*argument_type == WEBIDL_TYPE_ANY) {
                /* allowing any type needs no check */
                return 0;
        }

        outputf(outc,
                "\tif (%s_argc > %d) {\n", DLPFX, argidx);

        switch (*argument_type) {
        case WEBIDL_TYPE_STRING:
                /* coerce values to string */
                outputf(outc,
                        "\t\tif (!duk_is_string(ctx, %d)) {\n"
                        "\t\t\tduk_to_string(ctx, %d);\n"
                        "\t\t}\n", argidx, argidx);
                break;

        case WEBIDL_TYPE_BOOL:
                outputf(outc,
                        "\t\tif (!duk_is_boolean(ctx, %d)) {\n"
                        "\t\t\tif (duk_is_number(ctx, %d)) {\n"
                        "\t\t\t\tduk_to_boolean(ctx, %d);\n"
                        "\t\t\t} else {\n"
                        "\t\t\t\treturn duk_error(ctx, DUK_ERR_ERROR, %s_error_fmt_bool_type, %d, \"%s\");\n"
                        "\t\t\t}\n"
                        "\t\t}\n", argidx, argidx, argidx, DLPFX, argidx, argumente->name);
                break;

        case WEBIDL_TYPE_FLOAT:
        case WEBIDL_TYPE_DOUBLE:
        case WEBIDL_TYPE_SHORT:
        case WEBIDL_TYPE_LONG:
        case WEBIDL_TYPE_LONGLONG:
                outputf(outc,
                        "\t\tif (!duk_is_number(ctx, %d)) {\n"
                        "\t\t\treturn duk_error(ctx, DUK_ERR_ERROR, %s_error_fmt_number_type, %d, \"%s\");\n"
                        "\t\t}\n",
                        argidx, DLPFX, argidx, argumente->name);
                break;


        default:
                outputf(outc,
                        "\t\t/* unhandled type check */\n");
        }

        outputf(outc,
                "\t}\n");

        return 0;
}


/**
 * generate argument count and type checking for a simple operation
 */
static int
output_operation_arguments(struct opctx *outc,
                           struct ir_entry *interfacee,
                           struct ir_operation_entry *operatione)
{
        struct ir_operation_overload_entry *overloade;
        int fixedargc; /* number of non optional arguments */
        int argidx; /* loop counter for arguments */
        int optargc; /* loop counter for optional arguments */

        overloade = operatione->overloadv;

        fixedargc = overloade->argumentc - overloade->optionalc;

        outputf(outc,
                "\t/* ensure the parameters are present */\n"
                "\tduk_idx_t %s_argc = duk_get_top(ctx);\n\t", DLPFX);

        if (fixedargc > 0) {
                outputf(outc,
                        "if (%s_argc < %d) {\n",
                        DLPFX, fixedargc);
                outputf(outc,
                        "\t\t/* not enough arguments */\n");
                outputf(outc,
                        "\t\treturn duk_error(ctx, DUK_RET_TYPE_ERROR, %s_error_fmt_argument, %d, %s_argc);\n",
                        DLPFX, fixedargc, DLPFX);
                outputf(outc,
                        "\t} else ");
        }

        for (optargc = fixedargc;
             optargc < overloade->argumentc;
             optargc++) {
                outputf(outc,
                        "if (%s_argc == %d) {\n"
                        "\t\t/* %d optional arguments need adding */\n",
                        DLPFX,
                        optargc,
                        overloade->argumentc - optargc);
                output_operation_optional_defaults(outc,
                        overloade->argumentv + optargc,
                        overloade->argumentc - optargc);
                outputf(outc,
                        "\t} else ");
        }

        outputf(outc,
                "if (%s_argc > %d) {\n"
                "\t\t/* remove extraneous parameters */\n"
                "\t\tduk_set_top(ctx, %d);\n"
                "\t}\n",
                DLPFX,
                overloade->argumentc,
                overloade->argumentc);
        outputf(outc,
                "\n");

        /* generate argument type checks */

        outputf(outc,
                "\t/* check types of passed arguments are correct */\n");

        for (argidx = 0; argidx < overloade->argumentc; argidx++) {
                output_operation_argument_type_check(outc,
                                                     interfacee,
                                                     operatione,
                                                     overloade,
                                                     argidx);
        }

        return 0;
}


/**
 * generate a single class method for an interface operation
 */
static int
output_interface_operation(struct opctx *outc,
                           struct ir_entry *interfacee,
                           struct ir_operation_entry *operatione)
{
        int cdatac; /* cdata blocks output */

        if (operatione->name == NULL) {
                /* special method definition */
                outputf(outc,
                        "/* Special method definition - UNIMPLEMENTED */\n\n");

                WARN(WARNING_UNIMPLEMENTED,
                     "Special operation on interface %s (operation entry %p)",
                     interfacee->name,
                     operatione);
                return 0;
        }

        output_body_start(outc, interfacee, operatione->name);

        if (operatione->overloadc != 1) {
                /** \todo This is where the checking of the parameters
                 * to the overloaded operation should go
                 */
        } else if (operatione->overloadv->elipsisc != 0) {
                /**
                 * \todo This is where the checking of the parameters to
                 * the operation with elipsis should go
                 */
                WARN(WARNING_UNIMPLEMENTED,
                     "Elipsis parameters not checked: method %s::%s();",
                     interfacee->name, operatione->name);
        } else {
                output_operation_arguments(outc, interfacee, operatione);
        }

        cdatac = output_ccode(outc, operatione->method);
        if (cdatac == 0) {
                /* no implementation so generate default */
                WARN(WARNING_UNIMPLEMENTED,
                     "Unimplemented: method %s::%s();",
                     interfacee->name, operatione->name);

                if (options->dbglog) {
                        outputf(outc,
                                "\tNSLOG(qjsky, WARNING, \"Unimplemented\");\n");
                }

                outputf(outc,
                        "\treturn 0;\n");
        }

        outputf(outc,
                "}\n\n");

        return output_native_wrapper(outc, interfacee, operatione->name, 0);
}


/**
 * generate class methods for each interface operation
 */
static int
output_interface_operations(struct opctx *outc, struct ir_entry *ife)
{
        int opc;
        int res = 0;

        for (opc = 0; opc < ife->u.interface.operationc; opc++) {
                res = output_interface_operation(
                        outc,
                        ife,
                        ife->u.interface.operationv + opc);
                if (res != 0) {
                        break;
                }
        }

        return res;
}


/**
 * get a printable type of an attribute for warnings
 */
static const char *attribute_type_str(struct ir_attribute_entry *atributee)
{
        if (atributee->typec == 0) {
                return "";
        } else if (atributee->typec == 1) {
                return webidl_type_to_str(atributee->typev[0].modifier,
                                          atributee->typev[0].base);
        }
        return "multiple";
}


/**
 * Output class property getter for a single attribute
 */
static int
output_attribute_getter(struct opctx *outc,
                        struct ir_entry *interfacee,
                        struct ir_attribute_entry *atributee)
{
        char *name;
        int namelen;

        namelen = strlen(atributee->name) + 8;
        name = malloc(namelen);
        snprintf(name, namelen, "%s_getter", atributee->name);

        output_body_start(outc, interfacee, name);

        /* if binding available for this attribute getter process it */
        if (atributee->getter != NULL) {
                int res;
                res = output_ccode(outc, atributee->getter);
                if (res == 0) {
                        /* no code provided for this getter so generate */
                        res = output_generated_attribute_getter(outc,
                                                                interfacee,
                                                                atributee);
                }
                if (res >= 0) {
                        outputf(outc,
                                "}\n\n");
                        output_native_wrapper(outc, interfacee, name, 1);
                        free(name);
                        return res;
                }
        }

        /* no implementation so generate default and warnings if required */
        WARN(WARNING_UNIMPLEMENTED,
             "Unimplemented: getter %s::%s(%s);",
             interfacee->name,
             atributee->name,
             attribute_type_str(atributee));

        if (options->dbglog) {
                outputf(outc,
                        "\tNSLOG(qjsky, WARNING, \"Unimplemented\");\n" );
        }

        outputf(outc,
                "\treturn 0;\n"
                "}\n\n");

        output_native_wrapper(outc, interfacee, name, 1);
        free(name);

        return 0;
}


/**
 * Generate class property setter for a putforwards attribute
 */
static int
output_putforwards_setter(struct opctx *outc,
                          struct ir_entry *interfacee,
                          struct ir_attribute_entry *atributee)
{
        /* generate autogenerated putforwards */

        outputf(outc,
                "\tduk_ret_t get_ret;\n\n");

        outputf(outc,
                "\tget_ret = %s_%s_%s_getter(ctx, priv);\n",
                DLPFX, interfacee->class_name, atributee->name);

        outputf(outc,
                "\tif (get_ret != 1) {\n"
                "\t\treturn 0;\n"
                "\t}\n\n"
                "\t/* parameter ... attribute */\n\n"
                "\tduk_dup(ctx, 0);\n"
                "\t/* ... attribute parameter */\n\n"
                "\t/* call the putforward */\n");

        outputf(outc,
                "\tduk_put_prop_string(ctx, -2, \"%s\");\n\n",
                atributee->putforwards);

        outputf(outc,
                "\treturn 0;\n");

        return 0;
}


/**
 * Generate class property setter for a single attribute
 */
static int
output_attribute_setter(struct opctx *outc,
                        struct ir_entry *interfacee,
                        struct ir_attribute_entry *atributee)
{
        int res = -1;
        char *name;
        int namelen;

        namelen = strlen(atributee->name) + 8;
        name = malloc(namelen);
        snprintf(name, namelen, "%s_setter", atributee->name);

        output_body_start(outc, interfacee, name);

        /* if binding available for this attribute getter process it */
        if (atributee->setter != NULL) {
                res = output_ccode(outc, atributee->setter);
                if (res == 0) {
                        /* no code provided for this setter so generate */
                        res = output_generated_attribute_setter(outc,
                                                                interfacee,
                                                                atributee);
                }
        } else if (atributee->putforwards != NULL) {
                res = output_putforwards_setter(outc,
                                                interfacee,
                                                atributee);
        }

        /* implementation not generated from any other source */
        if (res < 0) {
                WARN(WARNING_UNIMPLEMENTED,
                     "Unimplemented: setter %s::%s(%s);",
                     interfacee->name,
                     atributee->name,
                     attribute_type_str(atributee));
                if (options->dbglog) {
                        outputf(outc,
                               "\tNSLOG(qjsky, WARNING, \"Unimplemented\");\n");
                }

                /* no implementation so generate default */
                outputf(outc,
                        "\treturn 0;\n");
        }

        outputf(outc,
                "}\n\n");

        output_native_wrapper(outc, interfacee, name, 2);
        free(name);

        return res;
}


/**
 * check if an attribute has a setter
 */
static bool attribute_has_setter(struct ir_attribute_entry *atributee)
{
        return (atributee->modifier != WEBIDL_TYPE_MODIFIER_READONLY) ||
                (atributee->putforwards != NULL);
}


/**
 * generate class property getters and setters for each interface attribute
 */
static int
output_interface_attributes(struct opctx *outc, struct ir_entry *ife)
{
        int attrc;

        for (attrc = 0; attrc < ife->u.interface.attributec; attrc++) {
                struct ir_attribute_entry *atributee;

                atributee = ife->u.interface.attributev + attrc;

                if (atributee->property_name == NULL) {
                        atributee->property_name =
                                gen_idl2c_name(atributee->name);
                }

                output_attribute_getter(outc, ife, atributee);

                /* only read/write and putforward attributes have a setter */
                if (attribute_has_setter(atributee)) {
                        output_attribute_setter(outc, ife, atributee);
                }
        }

        return 0;
}


/**
 * get the number of required arguments of an operation
 */
static int operation_length(struct ir_operation_entry *operatione)
{
        struct ir_operation_overload_entry *overloade;

        if ((operatione->overloadc != 1) ||
            (operatione->overloadv->elipsisc != 0)) {
                return 0;
        }

        overloade = operatione->overloadv;
        return overloade->argumentc - overloade->optionalc;
}


/**
 * generate the function list for an interface prototype
 *
 * \return the number of entries generated
 */
static int
output_prototype_funcs(struct opctx *outc, struct ir_entry *interfacee)
{
        int count = 0;
        int idx;

        outputf(outc,
                "static const JSCFunctionListEntry %s_%s___proto_funcs[] = {\n",
                QJPFX, interfacee->class_name);

        for (idx = 0; idx < interfacee->u.interface.operationc; idx++) {
                struct ir_operation_entry *operatione;

                operatione = interfacee->u.interface.operationv + idx;
                if (operatione->name == NULL) {
                        continue;
                }
                outputf(outc,
                        "\tJS_CFUNC_DEF2(\"%s\", %d, %s_%s_%s, JS_PROP_C_W_E),\n",
                        operatione->name,
                        operation_length(operatione),
                        QJPFX, interfacee->class_name, operatione->name);
                count++;
        }

        for (idx = 0; idx < interfacee->u.interface.attributec; idx++) {
                struct ir_attribute_entry *atributee;

                atributee = interfacee->u.interface.attributev + idx;
                if (attribute_has_setter(atributee)) {
                        outputf(outc,
                                "\tJS_CGETSET_DEF2(\"%s\", %s_%s_%s_getter, %s_%s_%s_setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE),\n",
                                atributee->name,
                                QJPFX, interfacee->class_name, atributee->name,
                                QJPFX, interfacee->class_name, atributee->name);
                } else {
                        outputf(outc,
                                "\tJS_CGETSET_DEF2(\"%s\", %s_%s_%s_getter, NULL, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE),\n",
                                atributee->name,
                                QJPFX, interfacee->class_name, atributee->name);
                }
                count++;
        }

        outputf(outc,
                "\tJS_PROP_STRING_DEF(\"[Symbol.toStringTag]\", \"%s\", JS_PROP_CONFIGURABLE),\n",
                interfacee->name);
        count++;

        outputf(outc, "};\n\n");

        return count;
}


/**
 * generate the function list of interface constants
 *
 * WebIDL constants are on both the interface object and its prototype.
 *
 * \return the number of entries generated
 */
static int
output_constant_funcs(struct opctx *outc, struct ir_entry *interfacee)
{
        int idx;

        if (interfacee->u.interface.constantc == 0) {
                return 0;
        }

        outputf(outc,
                "static const JSCFunctionListEntry %s_%s___const_funcs[] = {\n",
                QJPFX, interfacee->class_name);

        for (idx = 0; idx < interfacee->u.interface.constantc; idx++) {
                struct ir_constant_entry *constante;
                int *value;

                constante = interfacee->u.interface.constantv + idx;

                /** \todo This assumes the constant is a literal int and
                 * should check the type node base value.
                 */
                value = webidl_node_getint(
                        webidl_node_find_type(
                                webidl_node_getnode(constante->node),
                                NULL,
                                WEBIDL_NODE_TYPE_LITERAL_INT));

                outputf(outc,
                        "\tJS_PROP_INT32_DEF(\"%s\", %d, JS_PROP_ENUMERABLE),\n",
                        constante->name, *value);
        }

        outputf(outc, "};\n\n");

        return interfacee->u.interface.constantc;
}


/**
 * generate the prototype creation code from the binding, if any
 *
 * \return true if code was generated
 */
static bool
output_prototype_extra(struct opctx *outc, struct ir_entry *interfacee)
{
        struct genbind_node *proto_node;

        /* find the prototype method on the class */
        proto_node = genbind_node_find_method(interfacee->class,
                                              NULL,
                                              GENBIND_METHOD_TYPE_PROTOTYPE);
        if (proto_node == NULL) {
                return false;
        }

        outputf(outc,
                "static duk_ret_t %s_%s___proto_extra(duk_context *ctx, void *udata)\n"
                "{\n",
                QJPFX, interfacee->class_name);
        output_cdata(outc, proto_node, GENBIND_NODE_TYPE_CDATA);
        outputf(outc,
                "\treturn 0;\n"
                "}\n\n");

        return true;
}


/**
 * generate the class definition
 */
static int
output_interface_class(struct opctx *outc,
                       struct ir_entry *interfacee,
                       struct ir_entry *inherite)
{
        int proto_funcs_len;
        int const_funcs_len;
        bool extra;
        char *parent_name = NULL;

        proto_funcs_len = output_prototype_funcs(outc, interfacee);
        const_funcs_len = output_constant_funcs(outc, interfacee);
        extra = output_prototype_extra(outc, interfacee);

        if (inherite != NULL) {
                parent_name = qjs_proto_name(inherite->name);
        }

        outputf(outc,
                "const struct qjsky_class %s_%s___class = {\n"
                "\t.name = \"%s\",\n",
                QJPFX, interfacee->class_name,
                interfacee->name);

        if (parent_name != NULL) {
                outputf(outc, "\t.parent = QJSKY_PROTO_%s,\n", parent_name);
        } else {
                outputf(outc, "\t.parent = -1,\n");
        }

        outputf(outc,
                "\t.priv_size = sizeof(%s_private_t),\n"
                "\t.primary_global = %s,\n"
                "\t.construct = %s_%s___construct,\n"
                "\t.destroy = %s_%s___destroy,\n"
                "\t.proto_funcs = %s_%s___proto_funcs,\n"
                "\t.proto_funcs_len = %d,\n",
                interfacee->class_name,
                interfacee->u.interface.primary_global ? "true" : "false",
                QJPFX, interfacee->class_name,
                QJPFX, interfacee->class_name,
                QJPFX, interfacee->class_name,
                proto_funcs_len);

        if (const_funcs_len > 0) {
                outputf(outc,
                        "\t.const_funcs = %s_%s___const_funcs,\n"
                        "\t.const_funcs_len = %d,\n",
                        QJPFX, interfacee->class_name,
                        const_funcs_len);
        }

        if (extra) {
                outputf(outc,
                        "\t.proto_extra = %s_%s___proto_extra,\n",
                        QJPFX, interfacee->class_name);
        }

        outputf(outc, "};\n\n");

        free(parent_name);

        return 0;
}


/*
 * generate a source file to implement an interface using QuickJS and libdom.
 *
 * exported interface documented in qjs-libdom.h
 */
int qjs_output_interface(struct ir *ir, struct ir_entry *interfacee)
{
        struct opctx *ifop;
        struct ir_entry *inherite;
        int res = 0;

        /* open the output */
        res = output_open(interfacee->filename, &ifop);
        if (res !=0 ) {
                return res;
        }

        /* find parent interface entry */
        inherite = ir_inherit_entry(ir, interfacee);

        /* tool preface */
        output_tool_preface(ifop);

        /* binding preface */
        output_method_cdata(ifop,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_PREFACE);

        /* class preface */
        output_method_cdata(ifop,
                            interfacee->class,
                            GENBIND_METHOD_TYPE_PREFACE);

        /* tool prologue */
        output_tool_prologue(ifop);

        /* binding prologue */
        output_method_cdata(ifop,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_PROLOGUE);

        /* class prologue */
        output_method_cdata(ifop,
                            interfacee->class,
                            GENBIND_METHOD_TYPE_PROLOGUE);

        outputf(ifop,
                "\n");

        /* initialisor */
        res = output_interface_init(ifop, interfacee, inherite);
        if (res != 0) {
                goto op_error;
        }

        /* finaliser */
        output_interface_fini(ifop, interfacee, inherite);

        /* class table construct and destroy */
        output_interface_construct(ifop, interfacee);

        /* operations */
        output_interface_operations(ifop, interfacee);

        /* attributes */
        output_interface_attributes(ifop, interfacee);

        /* class definition */
        output_interface_class(ifop, interfacee, inherite);

        /* class epilogue */
        output_method_cdata(ifop,
                            interfacee->class,
                            GENBIND_METHOD_TYPE_EPILOGUE);

        /* binding epilogue */
        output_method_cdata(ifop,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_EPILOGUE);

        /* class postface */
        output_method_cdata(ifop,
                            interfacee->class,
                            GENBIND_METHOD_TYPE_POSTFACE);

        /* binding postface */
        output_method_cdata(ifop,
                            ir->binding_node,
                            GENBIND_METHOD_TYPE_POSTFACE);

op_error:
        output_close(ifop);

        return res;
}


/* exported function documented in qjs-libdom.h */
int
qjs_output_interface_declaration(struct opctx *outc, struct ir_entry *interfacee)
{
        struct genbind_node *init_node;

        /* do not generate declarations for interfaces marked no output */
        if (interfacee->u.interface.noobject) {
                return 0;
        }

        /* class definition declaration */
        outputf(outc,
                "extern const struct qjsky_class %s_%s___class;\n",
                QJPFX, interfacee->class_name);

        /* if the interface has no references (no other interface inherits from
         * it) there is no reason to export the initalisor/finaliser as no
         * other class constructor/destructor should call them.
         */
        if (interfacee->refcount < 1) {
                outputf(outc,
                        "\n");
                return 0;
        }

        /* finaliser declaration */
        outputf(outc,
                "void %s_%s___fini(duk_context *ctx, %s_private_t *priv);\n",
                DLPFX, interfacee->class_name, interfacee->class_name);

        /* find the initialisor method on the class (if any) */
        init_node = genbind_node_find_method(interfacee->class,
                                             NULL,
                                             GENBIND_METHOD_TYPE_INIT);

        /* initialisor definition */
        output_interface_init_declaration(outc, interfacee, init_node);

        outputf(outc,
                ";\n\n");

        return 0;
}
