/*
 Copyright (c) 2012, The Saffire Group
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
#include <string.h>
#include "compiler/bytecode.h"
#include "vm/vm.h"
#include "vm/vm_opcodes.h"
#include "vm/block.h"
#include "vm/frame.h"
#include "general/dll.h"
#include "general/smm.h"
#include "objects/object.h"
#include "objects/boolean.h"
#include "objects/string.h"
#include "objects/base.h"
#include "objects/numerical.h"
#include "objects/hash.h"
#include "objects/code.h"
#include "objects/method.h"
#include "objects/null.h"
#include "objects/userland.h"
#include "modules/module_api.h"
#include "debug.h"
#include "general/output.h"
#include "gc/gc.h"

t_hash_table *builtin_identifiers;         // Builtin identifiers like first-class objects, the _sfl etc

// A method-argument hash consists of name => structure
typedef struct _method_arg {
    t_object *value;
    t_string_object *typehint;
} t_method_arg;


#define OBJ2STR(_obj_) smm_strdup(((t_string_object *)_obj_)->value)
#define OBJ2NUM(_obj_) (((t_numerical_object *)_obj_)->value)

#define REASON_NONE         0
#define REASON_RETURN       1
#define REASON_CONTINUE     2
#define REASON_BREAK        3
#define REASON_BREAKELSE    4


/**
 * Parse calling arguments
 */
static void _parse_calling_arguments(t_vm_frame *cur_frame, t_vm_frame *new_frame, int arg_count, t_method_object *method) {
    // Check if the number of arguments given exceeds the number of arguments needed.
    t_hash_table *ht = ((t_hash_object *)method->arguments)->ht;
    if (arg_count > ht->element_count) {
        // @TODO: If we have variable argument count, this does not apply!
        error_and_die(1, "Too many arguments given\n");
    }

    // Iterate all arguments for this method.
    int csp = cur_frame->sp + arg_count - 1;
    int args_left = arg_count;
    int cur_arg = 1;

    t_hash_iter iter;
    ht_iter_init(&iter, ht);
    while (ht_iter_valid(&iter)) {
        char *name = ht_iter_key(&iter);
        t_method_arg *arg = ht_iter_value(&iter);

        // Set object to default value if any
        t_object *obj = (arg->value->type == objectTypeNull) ? NULL : arg->value;

        if (args_left) {
            // When arguments are left on the stack, override object with next value on stack
            obj = cur_frame->stack[csp--];      // Don't pop, just fetch
            args_left--;
        }

        // Found a value (either default or from stack)?
        if (obj == NULL) {
            error_and_die(1, "No value found for argument %d\n", cur_arg);
        }

        // Check for correct typehinting
        if (arg->typehint->type != objectTypeNull && strcmp(OBJ2STR(arg->typehint), obj->name) != 0) {
            error_and_die(1, "Typehinting for argument %d does not match. Wanted '%s' but found '%s'\n", cur_arg, OBJ2STR(arg->typehint), obj->name);
        }

        // Everything is ok, add the new value onto the local identifiers
        ht_add(new_frame->local_identifiers->ht, name, obj);

        // Process next argument
        cur_arg++;
        ht_iter_next(&iter);
    }
}



/**
 *
 */
void vm_init(void) {
    gc_init();
    builtin_identifiers = ht_create();
    object_init();
    module_init();
}

void vm_fini(void) {
    module_fini();
    object_fini();
    ht_destroy(builtin_identifiers);
    gc_fini();
}


/**
 *
 */
static t_object *_import(t_vm_frame *frame, char *module, char *class) {
    char tmp[100];
    snprintf(tmp, 99, "%s::%s", module, class);

    t_object *obj = vm_frame_get_identifier(frame, tmp);
    return obj;
}


/**
 *
 */
t_object *_vm_execute(t_vm_frame *frame) {
    register t_object *obj1, *obj2, *obj3, *obj4;
    register t_object *left_obj, *right_obj;
    register t_object *dst;
    register char *name;
    register unsigned int opcode, oparg1, oparg2;
    int reason = REASON_NONE;
    t_vm_frame *tfr;
    t_vm_frameblock *block;


    // Default return value;
    t_object *ret = Object_Null;

    for (;;) {
        // Room for some other stuff
dispatch:
        // Increase number of executions done
        frame->executions++;


#ifdef __DEBUG
        unsigned long cip = frame->ip;
#endif

        // Get opcode and additional argument
        opcode = vm_frame_get_next_opcode(frame);

        // If high bit is set, get operand
        oparg1 = ((opcode & 0x80) == 0x80) ? vm_frame_get_operand(frame) : 0;
        oparg2 = ((opcode & 0xC0) == 0xC0) ? vm_frame_get_operand(frame) : 0;

#ifdef __DEBUG
        if ((opcode & 0xC0) == 0xC0) {
            DEBUG_PRINT("%08lX : 0x%02X (0x%02X, 0x%02X)\n", cip, opcode, oparg1, oparg2);
        } else if ((opcode & 0x80) == 0x80) {
            DEBUG_PRINT("%08lX : 0x%02X (0x%02X)\n", cip, opcode, oparg1);
        } else {
            DEBUG_PRINT("%08lX : 0x%02X\n", cip, opcode);
        }
#endif

        if (opcode == VM_STOP) break;
        if (opcode == VM_RESERVED) {
            error_and_die(1, "VM: Reached reserved (0xFF) opcode. Halting.\n");
        }


        switch (opcode) {
            // Removes SP-0
            case VM_POP_TOP :
                obj1 = vm_frame_stack_pop(frame);
                object_dec_ref(obj1);
                goto dispatch;
                break;

            // Rotate / swap SP-0 and SP-1
            case VM_ROT_TWO :
                obj1 = vm_frame_stack_pop(frame);
                obj2 = vm_frame_stack_pop(frame);
                vm_frame_stack_push(frame, obj1);
                vm_frame_stack_push(frame, obj2);
                goto dispatch;
                break;

            // Rotate SP-0 to SP-2
            case VM_ROT_THREE :
                obj1 = vm_frame_stack_pop(frame);
                obj2 = vm_frame_stack_pop(frame);
                obj3 = vm_frame_stack_pop(frame);
                vm_frame_stack_push(frame, obj1);
                vm_frame_stack_push(frame, obj2);
                vm_frame_stack_push(frame, obj3);
                goto dispatch;
                break;

            // Duplicate SP-0
            case VM_DUP_TOP :
                obj1 = vm_frame_stack_fetch_top(frame);
                object_inc_ref(obj1);
                vm_frame_stack_push(frame, obj1);
                goto dispatch;
                break;

            // Rotate SP-0 to SP-3
            case VM_ROT_FOUR :
                obj1 = vm_frame_stack_pop(frame);
                obj2 = vm_frame_stack_pop(frame);
                obj3 = vm_frame_stack_pop(frame);
                obj4 = vm_frame_stack_pop(frame);
                vm_frame_stack_push(frame, obj1);
                vm_frame_stack_push(frame, obj2);
                vm_frame_stack_push(frame, obj3);
                vm_frame_stack_push(frame, obj4);
                goto dispatch;
                break;

            // No operation
            case VM_NOP :
                // Do nothing..
                goto dispatch;
                break;

            // Load a global identifier
            case VM_LOAD_GLOBAL :
                dst = vm_frame_get_global_identifier(frame, oparg1);
                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            // store SP+0 as a global identifier
            case VM_STORE_GLOBAL :
                // Refcount stays equal. So no inc/dec ref needed
                dst = vm_frame_stack_pop(frame);
                name = vm_frame_get_name(frame, oparg1);
                vm_frame_set_global_identifier(frame, name, dst);
                goto dispatch;
                break;

            // Remove global identifier
            case VM_DELETE_GLOBAL :
                dst = vm_frame_get_global_identifier(frame, oparg1);
                object_dec_ref(dst);
                name = vm_frame_get_name(frame, oparg1);
                vm_frame_set_global_identifier(frame, name, NULL);
                goto dispatch;
                break;

            // Load and push constant onto stack
            case VM_LOAD_CONST :
                dst = vm_frame_get_constant(frame, oparg1);
                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            // Store SP+0 into identifier (either local or global)
            case VM_STORE_ID :
                // Refcount stays equal. So no inc/dec ref needed
                dst = vm_frame_stack_pop(frame);
                register char *name = vm_frame_get_name(frame, oparg1);
                DEBUG_PRINT("Storing '%s' as '%s'\n", object_debug(dst), name);
                vm_frame_set_identifier(frame, name, dst);

                goto dispatch;
                break;

                // @TODO: If string(obj1) exists in local store it there, otherwise, store in global

            // Load and push identifier onto stack (either local or global)
            case VM_LOAD_ID :
                name = vm_frame_get_name(frame, oparg1);
                dst = vm_frame_get_identifier(frame, name);
                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            //
            case VM_OPERATOR :
                left_obj = vm_frame_stack_pop(frame);
                object_dec_ref(left_obj);
                right_obj = vm_frame_stack_pop(frame);
                object_dec_ref(right_obj);

                if (left_obj->type != right_obj->type) {
                    error_and_die(1, "Types are not equal. Coersing needed, but not yet implemented\n");
                }
                dst = object_operator(left_obj, oparg1, 0, 1, right_obj);

                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            case VM_INPLACE_OPR :
                left_obj = vm_frame_stack_pop(frame);
                object_dec_ref(left_obj);
                right_obj = vm_frame_stack_pop(frame);
                object_dec_ref(right_obj);

                if (left_obj->type != right_obj->type) {
                    error_and_die(1, "Types are not equal. Coersing needed, but not yet implemented\n");
                }
                dst = object_operator(left_obj, oparg1, 1, 1, right_obj);

                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            // Unconditional relative jump forward
            case VM_JUMP_FORWARD :
                frame->ip += oparg1;
                goto dispatch;
                break;

            // Conditional jump on SP-0 is true
            case VM_JUMP_IF_TRUE :
                dst = vm_frame_stack_fetch_top(frame);
                if (! OBJECT_IS_BOOLEAN(dst)) {
                    // Cast to boolean
                    register t_object *bool_method = object_find_method(dst, "boolean");
                    dst = object_call(dst, bool_method, 0);
                }

                if (IS_BOOLEAN_TRUE(dst)) {
                    frame->ip += oparg1;
                }

                goto dispatch;
                break;

            // Conditional jump on SP-0 is false
            case VM_JUMP_IF_FALSE :
                dst = vm_frame_stack_fetch_top(frame);
                if (! OBJECT_IS_BOOLEAN(dst)) {
                    // Cast to boolean
                    register t_object *bool_method = object_find_method(dst, "boolean");
                    dst = object_call(dst, bool_method, 0);
                }

                if (IS_BOOLEAN_FALSE(dst)) {
                    frame->ip += oparg1;
                }
                goto dispatch;
                break;

            case VM_JUMP_IF_FIRST_TRUE :
                dst = vm_frame_stack_fetch_top(frame);
                if (! OBJECT_IS_BOOLEAN(dst)) {
                    // Cast to boolean
                    register t_object *bool_method = object_find_method(dst, "boolean");
                    dst = object_call(dst, bool_method, 0);
                }

                // @TODO: We assume that this opcode has at least 1 block!
                if (IS_BOOLEAN_TRUE(dst) && frame->blocks[frame->block_cnt-1].visited == 0) {
                    frame->ip += oparg1;
                }

                // We have visited this frame
                frame->blocks[frame->block_cnt-1].visited = 1;
                goto dispatch;
                break;

            case VM_JUMP_IF_FIRST_FALSE :
                dst = vm_frame_stack_fetch_top(frame);
                if (! OBJECT_IS_BOOLEAN(dst)) {
                    // Cast to boolean
                    register t_object *bool_method = object_find_method(dst, "boolean");
                    dst = object_call(dst, bool_method, 0);
                }

                // @TODO: We assume that this opcode has at least 1 block!
                if (IS_BOOLEAN_FALSE(dst) && frame->blocks[frame->block_cnt-1].visited == 0) {
                    frame->ip += oparg1;
                }

                // We have visited this frame
                frame->blocks[frame->block_cnt-1].visited = 1;
                goto dispatch;
                break;

            // Unconditional absolute jump
            case VM_JUMP_ABSOLUTE :
                frame->ip = oparg1;
                goto dispatch;
                break;

            // Duplicates the SP+0 a number of times
            case VM_DUP_TOPX :
                dst = vm_frame_stack_fetch_top(frame);
                for (int i=0; i!=oparg1; i++) {
                    object_inc_ref(dst);
                    vm_frame_stack_push(frame, dst);
                }
                goto dispatch;
                break;

            // Calls method OP+0 SP+0 from object SP+1 with OP+1 args starting from SP+2.
            case VM_CALL_METHOD :
                {
                    // This object will become "self" in our method call
                    register t_object *self_obj = vm_frame_stack_pop(frame);

                    // Find the code from the method we need to call
                    register char *method_name = (char *)vm_frame_get_constant_literal(frame, oparg1);
                    t_method_object *method_obj = (t_method_object *)object_find_method(self_obj, method_name);
                    t_code_object *code_obj = (t_code_object *)method_obj->code;

                    if (OBJECT_TYPE_IS_CLASS(self_obj) && ! METHOD_IS_STATIC(method_obj)) {
                        error_and_die(1, "Cannot call dynamic method '%s' from a class\n", method_obj->name);
                    }


                    // @TODO: Native and userland functions are treated differently. This needs to be moved
                    //        to the code-object AND both systems need the same way to deal with arguments.

                    // Check if it's a native function.
                    if (code_obj->native_func) {

                        // Create argument list inside a DLL
                        t_dll *arg_list = dll_init();
                        for (int i=0; i!=oparg2; i++) {
                            dll_prepend(arg_list, vm_frame_stack_pop(frame));
                        }

                        // Call native function
                        dst = code_obj->native_func(self_obj, arg_list);

                        // Free argument list
                        dll_free(arg_list);
                    } else {
                        // Create a new executio frame
                        tfr = vm_frame_new(frame, code_obj->bytecode);

                        // Add references to parent and self
                        ht_replace(tfr->local_identifiers->ht, "self", self_obj);
                        ht_replace(tfr->local_identifiers->ht, "parent", self_obj->parent);

                        // Parse calling arguments to see if they match our signatures
                        _parse_calling_arguments(frame, tfr, oparg2, method_obj);

                        // "Remove" the arguments from the original stack
                        frame->sp += oparg2;

                        // Execute frame, return the last object
                        dst = _vm_execute(tfr);

                        // Destroy frame
                        vm_frame_destroy(tfr);
                    }

                    object_inc_ref(dst);
                    vm_frame_stack_push(frame, dst);
                }

                goto dispatch;
                break;

            // Import X as Y from Z
            case VM_IMPORT :
                {
                    // Fetch the module to import
                    register t_object *module_obj = vm_frame_stack_pop(frame);
                    object_dec_ref(module_obj);
                    register char *module_name = OBJ2STR(module_obj);

                    // Fetch class
                    register t_object *class_obj = vm_frame_stack_pop(frame);
                    object_dec_ref(class_obj);
                    register char *class_name = OBJ2STR(class_obj);

                    dst = _import(frame, module_name, class_name);
                    object_inc_ref(dst);
                    vm_frame_stack_push(frame, dst);
                }

                goto dispatch;
                break;


            case VM_SETUP_LOOP :
                vm_push_block(frame, BLOCK_TYPE_LOOP, frame->ip + oparg1, frame->sp, 0);
                goto dispatch;
                break;
            case VM_SETUP_ELSE_LOOP :
                vm_push_block(frame, BLOCK_TYPE_LOOP, frame->ip + oparg1, frame->sp, frame->ip + oparg2);
                goto dispatch;
                break;

            case VM_POP_BLOCK :
                vm_pop_block(frame);
                goto dispatch;
                break;

            case VM_CONTINUE_LOOP :
                ret = object_new(Object_Numerical, oparg1);
                reason = REASON_CONTINUE;
                goto block_end;
                break;
            case VM_BREAK_LOOP :
                reason = REASON_BREAK;
                goto block_end;
                break;
            case VM_BREAKELSE_LOOP :
                reason = REASON_BREAKELSE;
                goto block_end;
                break;

            case VM_COMPARE_OP :
                left_obj = vm_frame_stack_pop(frame);
                right_obj = vm_frame_stack_pop(frame);

                if (left_obj->type != right_obj->type) {
                    error_and_die(1, "Cannot compare non-identical object types\n");
                }
                dst = object_comparison(left_obj, oparg1, right_obj);

                object_dec_ref(left_obj);
                object_dec_ref(right_obj);

                object_inc_ref(dst);
                vm_frame_stack_push(frame, dst);
                goto dispatch;
                break;

            case VM_SETUP_FINALLY :
                vm_push_block(frame, BLOCK_TYPE_FINALLY, frame->ip + oparg1, frame->sp, 0);
                goto dispatch;
                break;
            case VM_SETUP_EXCEPT :
                vm_push_block(frame, BLOCK_TYPE_EXCEPTION, frame->ip + oparg1, frame->sp, 0);
                goto dispatch;
                break;
            case VM_END_FINALLY :
                //
                goto dispatch;
                break;


            case VM_BUILD_CLASS :
                {
                    // pop class name
                    register t_object *name = vm_frame_stack_pop(frame);
                    object_dec_ref(name);
                    DEBUG_PRINT("Name of the class: %s\n", OBJ2STR(name));

                    // pop flags
                    register t_object *flags = vm_frame_stack_pop(frame);
                    object_dec_ref(flags);
                    DEBUG_PRINT("Flags: %ld\n", OBJ2NUM((t_numerical_object *)flags));

                    // pop parent code object (as string)
                    register t_object *parent_class_obj = vm_frame_stack_pop(frame);
                    object_dec_ref(parent_class_obj);

                    // If no parent class has been given, use the Base class as parent
                    register t_object *parent_class;
                    if (OBJECT_IS_NULL(parent_class_obj)) {
                        parent_class = Object_Base;
                    } else {
                        // Find the object of this string
                        parent_class = vm_frame_get_identifier(frame, OBJ2STR((t_string_object *)parent_class_obj));
                    }
                    object_inc_ref(parent_class);

                    DEBUG_PRINT("Parent: %s\n", object_debug(parent_class));

                    //register t_object *class = object_new(Object_Userland, OBJ2STR(name), OBJ2NUM(flags), parent_class);

                    register t_object *class = (t_object *)smm_malloc(sizeof(t_object));
                    memcpy(class, Object_Userland, sizeof(t_userland_object));

                    class->name = smm_strdup(OBJ2STR(name));
                    class->flags = OBJ2NUM(flags) | OBJECT_TYPE_CLASS;
                    class->parent = parent_class;

                    // Iterate all methods
                    for (int i=0; i!=oparg1; i++) {
                        register t_object *name = vm_frame_stack_pop(frame);
                        register t_object *method = vm_frame_stack_pop(frame);

                        // add to class
                        ht_add(class->methods, OBJ2STR(name), method);

                        DEBUG_PRINT("Added method '%s' to class\n", object_debug(method));
                    }

                    object_inc_ref(class);
                    vm_frame_stack_push(frame, class);
                }

                goto dispatch;
                break;
            case VM_BUILD_METHOD :
                {

                    printf("\n\n\n**** CREATING A METHOD ****\n\n\n\n");

                    // pop flags object
                    register t_object *flags = vm_frame_stack_pop(frame);
                    object_dec_ref(flags);

                    // pop code object
                    register t_object *code_obj = vm_frame_stack_pop(frame);
                    object_dec_ref(code_obj);

                    // Generate hash object from arguments
                    register t_object *arg_list = object_new(Object_Hash, NULL);
                    for (int i=0; i!=oparg1; i++) {
                        t_method_arg *arg = smm_malloc(sizeof(t_method_arg));
                        arg->value = vm_frame_stack_pop(frame);
                        register t_object *name_obj = vm_frame_stack_pop(frame);
                        arg->typehint = (t_string_object *)vm_frame_stack_pop(frame);

                        ht_add(((t_hash_object *)arg_list)->ht, OBJ2STR(name_obj), arg);
                    }

                    // Generate method object
                    dst = object_new(Object_Method, flags, 0, NULL, code_obj, arg_list);

                    // Push method object
                    object_inc_ref(dst);
                    vm_frame_stack_push(frame, dst);
                }

                goto dispatch;
                break;
            case VM_RETURN :
                ret = vm_frame_stack_pop(frame);
                object_dec_ref(ret);

                reason = REASON_RETURN;
                goto block_end;
                break;

        } // switch(opcode) {


block_end:
        // We have reached the end of a frameblock or frame. Only use the RET object from here on.

        while (reason != REASON_NONE && frame->block_cnt > 0) {
            block = vm_fetch_block(frame);

            if (reason == REASON_CONTINUE && block->type == BLOCK_TYPE_LOOP) {
                DEBUG_PRINT("\n*** Continuing loop at %08lX\n\n", OBJ2NUM(ret));
                // Continue block
                frame->ip = OBJ2NUM(ret);
                break;
            }

            // Pop block. Not needed anymore.
            vm_pop_block(frame);

            // Unwind the stack. Make sure we are at the same level as the caller block.
            while (frame->sp < block->sp) {
                DEBUG_PRINT("Current SP: %d -> Needed SP: %d\n", frame->sp, block->sp);
                dst = vm_frame_stack_pop(frame);
                object_dec_ref(dst);
            }

            if (reason == REASON_BREAKELSE && block->type == BLOCK_TYPE_LOOP) {
                DEBUG_PRINT("\nBreaking loop to %08X\n\n", block->ip_else);
                frame->ip = block->ip_else;
                break;
            }

            if (reason == REASON_BREAK && block->type == BLOCK_TYPE_LOOP) {
                DEBUG_PRINT("\nBreaking loop to %08X\n\n", block->ip);
                frame->ip = block->ip;
                break;
            }
        }

        if (reason == REASON_RETURN) {
            // Break from the loop. We're done
            break;
        }

    } // for (;;)


    return ret;
}


/**
 *
 */
int vm_execute(t_bytecode *bc) {
    // Create initial frame
    t_vm_frame *initial_frame = vm_frame_new((t_vm_frame *) NULL, bc);

    // Execute the frame
    t_object *result = _vm_execute(initial_frame);

    DEBUG_PRINT("*** Vm execution done\n");

    // Convert returned object to numerical, so we can use it as an error code
    if (!OBJECT_IS_NUMERICAL(result)) {
        // Cast to numericak
        t_object *result_numerical = object_find_method(result, "numerical");
        result = object_call(result, result_numerical, 0);
    }
    int ret_val = ((t_numerical_object *) result)->value;

    vm_frame_destroy(initial_frame);
    return ret_val;
}

/**
 *
 */
void vm_populate_builtins(const char *name, void *data) {
    DEBUG_PRINT("Added object to builtins: %s\n", name);
    ht_add(builtin_identifiers, name, data);
}