/*
 Copyright (c) 2012-2013, The Saffire Group
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
     * Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
     * Neither the name of the <organization> nor the
       names of its contributors may be used to endorse or promote products
       derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "general/output.h"
#include "compiler/compiler.h"
#include "compiler/parser.tab.h"
#include "general/smm.h"
#include "compiler/ast.h"

//extern void yylex_destroy
extern int yylex_destroy();
extern void yyerror(t_ast_element *ast, const char *err);
extern int yyparse(unsigned long *ptr);
extern FILE *yyin;
extern int yylineno;

#ifdef __PARSEDEBUG
extern int yydebug;
extern int yy_flex_debug;
#endif



/**
 * Compile a a file into an AST (through bison). Returns the AST root node.
 */
t_ast_element *ast_generate_tree(FILE *fp) {
    // Initialize system
    sfc_init();

#ifdef __PARSEDEBUG
    yydebug = 1;
    yy_flex_debug = 1;
#endif

    // Parse the file input, will return the tree in the global ast_root variable
    yyin = fp;

    unsigned long ptr = 0;
    yyparse(&ptr);
    yylex_destroy();

    t_ast_element *ast = (t_ast_element *)ptr;

    sfc_fini();

    // Returning a global var. We should change this by having the root node returned by yyparse() if this is possible
    return ast;
}


/**
 * Allocate a new element
 */
static t_ast_element *ast_alloc_element(void) {
    t_ast_element *p;

    if ((p = smm_malloc(sizeof(t_ast_element))) == NULL) {
        yyerror(p, "Out of memory");   /* LCOV_EXCL_LINE */
    }
    p->lineno = yylineno;
    p->clean_handler = NULL;

    return p;
}

t_ast_element *ast_boolop(int boolop, t_ast_element *left, t_ast_element *right) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstBool;
    p->boolop.op = boolop;
    p->boolop.l = left;
    p->boolop.r = right;

    return p;
}
t_ast_element *ast_assignment(int op, t_ast_element *left, t_ast_element *right) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstAssignment;
    p->assignment.op = op;
    p->assignment.l = left;
    p->assignment.r = right;

    return p;
}
t_ast_element *ast_comparison(int cmp, t_ast_element *left, t_ast_element *right) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstComparison;
    p->comparison.cmp = cmp;
    p->comparison.l = left;
    p->comparison.r = right;

    return p;
}
t_ast_element *ast_operator(int op, t_ast_element *left, t_ast_element *right) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstOperator;
    p->operator.op = op;
    p->operator.l = left;
    p->operator.r = right;

    return p;
}

/**
 * Creates a string node
 */
t_ast_element *ast_string(char *value) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstString;
    p->string.value = smm_strdup(value);

    return p;
}


t_ast_element *ast_string_dup(t_ast_element *src) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstString;
    p->string.value = smm_strdup(src->string.value);

    return p;
}



/**
 * Creates a numerical node
 */
t_ast_element *ast_numerical(int value) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstNumerical;
    p->numerical.value = value;

    return p;
}


/**
 * Creates a identifier node
 */
t_ast_element *ast_identifier(char *var_name) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstIdentifier;
    p->identifier.name = smm_strdup(var_name);
    return p;
}

/**
 * Creates a identifier node
 */
t_ast_element *ast_property(t_ast_element *class, t_ast_element *property) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstProperty;
    p->property.class = class;
    p->property.property = property;

    return p;
}


/**
 * Creates a nop node
 */
t_ast_element *ast_nop(void) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstNop;
    return p;
}

/**
 * Creates a null node
 */
t_ast_element *ast_null(void) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstNull;
    return p;
}


/**
 * Add a node to an existing operator node. This allows to have multiple children in later stages (like lists)
 */
t_ast_element *ast_add(t_ast_element *src, t_ast_element *new_element) {
    if (src->type == typeAstGroup) {
        // Resize memory
        src->group.items = smm_realloc(src->group.items, (src->group.len+1) * sizeof(t_ast_element));
        if (src->group.items == NULL) {
            yyerror(src, "Out of memory");   /* LCOV_EXCL_LINE */
        }

        // Add new operator
        src->group.items[src->group.len] = new_element;
        src->group.len++;

        return src;
    }

    if (src->type == typeAstOpr) {
        // Resize memory
        src->opr.ops = smm_realloc(src->opr.ops, (src->opr.nops+1) * sizeof(t_ast_element));
        if (src->opr.ops == NULL) {
            yyerror(src, "Out of memory");   /* LCOV_EXCL_LINE */
        }

        // Add new operator
        src->opr.ops[src->opr.nops] = new_element;
        src->opr.nops++;

        return src;
    }

    yyerror(src, "Cannot add to non-grouping element");   /* LCOV_EXCL_LINE */
    return NULL;
}


///**
// * Add all the children of a node to the src node.
// */
//t_ast_element *ast_add_children(t_ast_element *src, t_ast_element *new_element) {
//    if (src->type != typeAstOpr) {
//        yyerror(src, "Cannot add to non-opr element");   /* LCOV_EXCL_LINE */
//    }
//    if (new_element->type != typeAstOpr) {
//        yyerror(src, "We can only add child elements from a opr element");   /* LCOV_EXCL_LINE */
//    }
//
//    // Allocate or resize operator memory
//    if (src->opr.ops) {
//        src->opr.ops = smm_realloc(src->opr.ops, (src->opr.nops + new_element->opr.nops) * sizeof(t_ast_element));
//    } else {
//        src->opr.ops = smm_malloc(new_element->opr.nops * sizeof(t_ast_element));
//    }
//    if (src->opr.ops == NULL) {
//        yyerror(src, "Out of memory");   /* LCOV_EXCL_LINE */
//    }
//
//    // Add new operator
//    for (int i=0; i!=new_element->opr.nops; i++) {
//        src->opr.ops[src->opr.nops] = new_element->opr.ops[i];
//        src->opr.nops++;
//    }
//
//    return src;
//}

t_ast_element *ast_group(int len, ...) {
    t_ast_element *p = ast_alloc_element();
    va_list ap;

    p->type = typeAstGroup;
    p->group.len = len;
    p->group.items = NULL;

    if (len && (p->group.items = smm_malloc (len * sizeof(t_ast_element))) == NULL) {
        yyerror(p, "Out of memory");   /* LCOV_EXCL_LINE */
    }

    // Add additional nodes (they can be added later with ast_add())
    if (len) {
        va_start(ap, len);
        for (int i=0; i < len; i++) {
            p->group.items[i] = va_arg(ap, t_ast_element *);
        }
        va_end(ap);
    }

    return p;
}


/**
 * Create an operator node
 */
t_ast_element *ast_opr(int opr, int nops, ...) {
    t_ast_element *p = ast_alloc_element();
    va_list ap;

    p->type = typeAstOpr;
    p->opr.oper = opr;
    p->opr.nops = nops;
    p->opr.ops = NULL;

    if (nops && (p->opr.ops = smm_malloc (nops * sizeof(t_ast_element))) == NULL) {
        yyerror(p, "Out of memory");   /* LCOV_EXCL_LINE */
    }

    // Add additional nodes (they can be added later with ast_add())
    if (nops) {
        va_start(ap, nops);
        for (int i=0; i < nops; i++) {
            p->opr.ops[i] = va_arg(ap, t_ast_element *);
        }
        va_end(ap);
    }

    return p;
}


/**
 * Concatenates an identifier node onto an existing identifier node
 */
t_ast_element *ast_concat(t_ast_element *src, char *s) {
    src->identifier.name= smm_realloc(src->identifier.name, strlen(src->identifier.name) + strlen(s) + 1);
    strcat(src->identifier.name, s);
    return src;
}


/**
 * Concatenates an string node onto an existing string node
 */
t_ast_element *ast_string_concat(t_ast_element *src, char *s) {
    src->string.value = smm_realloc(src->string.value, strlen(src->string.value) + strlen(s) + 1);
    strcat(src->string.value, s);
    return src;
}


/**
 * Create a class node.
 */
t_ast_element *ast_class(t_class *class, t_ast_element *body) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstClass;
    p->class.modifiers = class->modifiers;
    p->class.name = smm_strdup(class->name);

    p->class.extends = class->extends;
    p->class.implements = class->implements;

    p->class.body = body;

    return p;
}


/**
 * Create an interface node
 */
t_ast_element *ast_interface(int modifiers, char *name, t_ast_element *implements, t_ast_element *body) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstInterface;
    p->interface.modifiers = modifiers;
    p->interface.name = smm_strdup(name);
    p->interface.implements = implements;
    p->interface.body = body;

    return p;
}


/**
 * Create a attribute node
 */
t_ast_element *ast_attribute(char *name, char attrib_type, char visibility, char access, t_ast_element *value, char method_flags, t_ast_element *arguments) {
    t_ast_element *p = ast_alloc_element();

    p->type = typeAstAttribute;
    p->attribute.name = smm_strdup(name);
    p->attribute.attrib_type = attrib_type;
    p->attribute.visibility = visibility;
    p->attribute.access = access;
    p->attribute.value = value;
    p->attribute.method_flags = method_flags;
    p->attribute.arguments = arguments;

    return p;
}


/**
 * Free up an AST node
 */
void ast_free_node(t_ast_element *p) {
    if (!p) return;

    switch (p->type) {
        case typeAstNop :
        case typeAstNull :
        case typeAstNumerical :
            // Nothing to free
            break;
        case typeAstAttribute :
            smm_free(p->attribute.name);
            ast_free_node(p->attribute.value);
            ast_free_node(p->attribute.arguments);
            break;
        case typeAstProperty :
            ast_free_node(p->property.class);
            ast_free_node(p->property.property);
            break;
        case typeAstOperator :
            ast_free_node(p->operator.r);
            ast_free_node(p->operator.l);
            break;
        case typeAstBool :
            ast_free_node(p->boolop.r);
            ast_free_node(p->boolop.l);
            break;
        case typeAstAssignment :
            ast_free_node(p->assignment.r);
            ast_free_node(p->assignment.l);
            break;
        case typeAstComparison:
            ast_free_node(p->comparison.r);
            ast_free_node(p->comparison.l);
            break;
        case typeAstString :
            smm_free(p->string.value);
            break;
        case typeAstIdentifier :
            smm_free(p->identifier.name);
            break;
        case typeAstClass :
            smm_free(p->class.name);
            ast_free_node(p->class.extends);
            ast_free_node(p->class.implements);
            ast_free_node(p->class.body);
            break;
        case typeAstInterface :
            smm_free(p->interface.name);
            ast_free_node(p->interface.implements);
            ast_free_node(p->interface.body);
            break;
        case typeAstOpr :
            if (p->opr.nops) {
                for (int i=0; i < p->opr.nops; i++) {
                    ast_free_node(p->opr.ops[i]);
                }
                smm_free(p->opr.ops);
            }
            break;
        case typeAstGroup :
            if (p->group.len) {
                for (int i=0; i < p->group.len; i++) {
                    ast_free_node(p->group.items[i]);
                }
                smm_free(p->group.items);
            }
            break;

    }
    smm_free(p);
}


/**
 * Generate an AST from a source file
 */
t_ast_element *ast_generate_from_file(const char *source_file) {
    // Open file, or use stdin if needed
    FILE *fp = (! strcmp(source_file,"-") ) ? stdin : fopen(source_file, "r");
    if (!fp) {
        error("Could not open file: %s\n", source_file);
        return NULL;
    }

    // Generate source file into an AST tree
    t_ast_element *ast = ast_generate_tree(fp);

    // Close file
    fclose(fp);

    return ast;
}