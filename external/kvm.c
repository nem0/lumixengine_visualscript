#include "kvm.h"
#include <stdbool.h>
#include <string.h>

#ifdef _MSC_VER
	#pragma warning(error : 4062)
#endif

typedef enum KVM_OP {
	// end execution
	KVM_OP_END,
	
	// call syscall with fixed number of args, pop the args after
	KVM_OP_SYSCALL,

	// push 32bit constant to stack
	KVM_OP_CONST32,

	// push 64bit constant to stack
	KVM_OP_CONST64,

	// pop from
	KVM_OP_POP,

	// skip next instruction (must be jmp) if stack[-1] == stack[-2]
	KVM_OP_EQ,

	// skip next instruction (must be jmp) if stack[-1] != stack[-2]
	KVM_OP_NEQ,

	// skip next instruction (must be jmp) if stack[-2] < stack[-1]
	KVM_OP_LT,
	// float version
	KVM_OP_LTF,

	// skip next instruction (must be jmp) if stack[-2] > stack[-1]
	KVM_OP_GT,
	// float version
	KVM_OP_GTF,

	// see kvm_bc_add
	KVM_OP_ADD,
	
	// see kvm_bc_addf
	KVM_OP_ADDF,
	
	// see kvm_bc_mul
	KVM_OP_MUL,
	
	// see kvm_bc_mulf
	KVM_OP_MULF,
	
	// jump to label
	KVM_OP_JMP,
	
	// pop return address and jump to it
	KVM_OP_RET,
	
	// push return address and jump
	KVM_OP_CALL,
	
	// push(stack[idx])
	KVM_OP_GET_LOCAL,
	
	// push(env[idx])
	KVM_OP_GET,
	
	// env[idx] = stack[-1]
	KVM_OP_SET,
} KVM_OP;

kvm_u32 kvm_get(KVM* vm, kvm_i32 idx) {
	kvm_u32 res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx, sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx, sizeof(res));
	}
	return res;
}

void* kvm_get_ptr(KVM* vm, kvm_i32 idx) {
	void *res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx, sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx, sizeof(res));
	}
	return res;

}

kvm_u64 kvm_get64(KVM* vm, kvm_i32 idx) {
	kvm_u64 res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx, sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx, sizeof(res));
	}
	return res;
}

float kvm_get_float(KVM* vm, kvm_i32 idx) {
	float res;
	memcpy(&res, vm->stack + vm->sp + idx, sizeof(res));
	return res;
}


void kvm_push(KVM* vm, kvm_u32 value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	++vm->sp;
}

void kvm_push_float(KVM* vm, float value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	++vm->sp;
}

void kvm_push_ptr(KVM* vm, void* value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	vm->sp += sizeof(value) / sizeof(kvm_u32);
}

void kvm_init(KVM* vm, kvm_u32* environment, kvm_u32 environment_size_bytes) {
	vm->sp = 0;
	vm->environment = environment;
	vm->environment_size_bytes = environment_size_bytes;
}

void kvm_call(KVM* vm, const kvm_u8* bytecode, kvm_syscall syscall, kvm_label label) {
	
	#define READ(v) \
		do { \
			memcpy(&(v), ip, sizeof(v)); \
			ip += sizeof(v); \
		} while(false)

	const kvm_u8* ip = bytecode + label; 
	kvm_u32 sp = vm->sp;
	bool finished = false;
	while (!finished) {
		KVM_OP op;
		READ(op);
		switch(op) {
			case KVM_OP_END: finished = true; break;
			case KVM_OP_SYSCALL: {
				kvm_u32 arg_count;
				READ(arg_count);
				vm->sp = sp;
				syscall(vm, arg_count);
				sp = vm->sp;
				sp -= arg_count * sizeof(kvm_u32);
				break;
			}
			case KVM_OP_CONST32: 
				memcpy(vm->stack + sp, ip, sizeof(kvm_u32));
				ip += sizeof(kvm_u32);
				++sp;
				break;
			case KVM_OP_CONST64: {
				memcpy(vm->stack + sp, ip, sizeof(kvm_u64));
				ip += sizeof(kvm_u64);
				sp += 2;
				break;
			}
			case KVM_OP_RET: {
				kvm_u32 addr;
				--sp;
				memcpy(&addr, vm->stack + sp, sizeof(addr));
				ip = bytecode + addr;
				break;
			}
			case KVM_OP_POP:
				--sp;
				break;
			case KVM_OP_CALL: {
				kvm_u32 func_addr;
				READ(func_addr);

				kvm_u32 ret_addr = (kvm_u32)(ip - bytecode);
				memcpy(vm->stack + sp, &ret_addr, sizeof(ret_addr));
				++sp;
				ip = bytecode + func_addr;
				break;
			}
			case KVM_OP_SET: {
				kvm_i32 idx;
				READ(idx);
				--sp;
				kvm_u32 v = vm->stack[sp];
				memcpy(vm->environment + idx, &v, sizeof(v));
				break;
			}
			case KVM_OP_GET: {
				kvm_i32 idx;
				READ(idx);
				memcpy(vm->stack + sp, vm->environment + idx, sizeof(kvm_u32));
				++sp;
				break;
			}
			case KVM_OP_GET_LOCAL: {
				kvm_i32 idx;
				READ(idx);
				memcpy(vm->stack + sp, vm->stack + idx, sizeof(kvm_u32));
				++sp;
				break;
			}
			case KVM_OP_JMP: {
				kvm_label label;
				READ(label);
				ip = bytecode + label;
				break;
			}
			case KVM_OP_MUL:
				--sp;
				vm->stack[sp - 1] *= vm->stack[sp];
				break;
			case KVM_OP_MULF:
				--sp;
				*(float*)(vm->stack + sp - 1) *= *(float*)(vm->stack + sp);
				break;
			case KVM_OP_ADD:
				--sp;
				vm->stack[sp - 1] += vm->stack[sp];
				break;
			case KVM_OP_ADDF:
				--sp;
				*(float*)(vm->stack + sp - 1) += *(float*)(vm->stack + sp);
				break;
			case KVM_OP_EQ:
				sp -= 2;
				if (vm->stack[sp] == vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
			case KVM_OP_NEQ:
				sp -= 2;
				if (vm->stack[sp] != vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
			case KVM_OP_GT:
				sp -= 2;
				if (vm->stack[sp] > vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
			case KVM_OP_LT:
				sp -= 2;
				if (vm->stack[sp] < vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
			case KVM_OP_GTF:
				sp -= 2;
				if (*(float*)&vm->stack[sp] > *(float*)&vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
			case KVM_OP_LTF:
				if (*(float*)&vm->stack[sp] < *(float*)&vm->stack[sp + 1]) {
					ip += sizeof(KVM_OP) + sizeof(kvm_label);
				}
				break;
		}
	}
	#undef READ
}

void kvm_bc_end_write(kvm_bc_writer* writer) {
	#define READ(v) \
		do { \
			memcpy(&(v), ip, sizeof(v)); \
			ip += sizeof(v); \
		} while(false)

	kvm_u8* ip = writer->bytecode;
	while (ip != writer->ip) {
		KVM_OP op;
		READ(op);
		switch(op) {
			case KVM_OP_RET:
			case KVM_OP_POP:
			case KVM_OP_MUL:
			case KVM_OP_MULF:
			case KVM_OP_ADD:
			case KVM_OP_ADDF:
			case KVM_OP_EQ:
			case KVM_OP_NEQ:
			case KVM_OP_LTF:
			case KVM_OP_LT:
			case KVM_OP_GTF:
			case KVM_OP_GT:
			case KVM_OP_END:
				break;
			case KVM_OP_JMP:
			case KVM_OP_CALL: {
				kvm_label label;
				READ(label);
				memcpy(ip - sizeof(label), writer->labels + label, sizeof(writer->labels[label]));
				break;
			}
			case KVM_OP_CONST64:
				ip += sizeof(kvm_u64);
				break;
			case KVM_OP_SYSCALL:
			case KVM_OP_GET_LOCAL:
			case KVM_OP_GET:
			case KVM_OP_SET:
			case KVM_OP_CONST32:
				ip += sizeof(kvm_u32);
				break;
		}
	}
	#undef READ
}


void kvm_bc_start_write(kvm_bc_writer* writer, kvm_u8* bytecode, kvm_u32 capacity) {
	writer->bytecode = bytecode;
	writer->capacity = capacity;
	writer->ip = bytecode;
	writer->labels_count = 0;
}

#define DEFINE_SIMPLE_OP_WRITER(op, OP) \
	void kvm_bc_##op(kvm_bc_writer* writer) { \
		if (writer->capacity < sizeof(KVM_OP_##OP)) { \
			return; \
		} \
		KVM_OP p = KVM_OP_##OP; \
		memcpy(writer->ip, &p, sizeof(p)); \
		writer->ip += sizeof(p); \
	}

#define DEFINE_SIMPLE_OP_WRITER_U32(op, OP) \
	void kvm_bc_##op(kvm_bc_writer* writer, kvm_u32 arg) { \
		kvm_u32 size = sizeof(KVM_OP) + sizeof(arg); \
		if (writer->capacity < size) return; \
		KVM_OP p = KVM_OP_##OP; \
		memcpy(writer->ip, &p, sizeof(p)); \
		memcpy(writer->ip + sizeof(p), &arg, sizeof(arg)); \
		writer->ip += size;	\
	}

	DEFINE_SIMPLE_OP_WRITER(end, END);
	DEFINE_SIMPLE_OP_WRITER(pop, POP);
	DEFINE_SIMPLE_OP_WRITER(add, ADD);
	DEFINE_SIMPLE_OP_WRITER(addf, ADDF);
	DEFINE_SIMPLE_OP_WRITER(mul, MUL);
	DEFINE_SIMPLE_OP_WRITER(mulf, MULF);
	DEFINE_SIMPLE_OP_WRITER(ret, RET);
	DEFINE_SIMPLE_OP_WRITER(eq, EQ);
	DEFINE_SIMPLE_OP_WRITER(neq, NEQ);
	DEFINE_SIMPLE_OP_WRITER(gt, GT);
	DEFINE_SIMPLE_OP_WRITER(gtf, GTF);
	DEFINE_SIMPLE_OP_WRITER(lt, LT);
	DEFINE_SIMPLE_OP_WRITER(ltf, LTF);

	DEFINE_SIMPLE_OP_WRITER_U32(jmp, JMP);
	DEFINE_SIMPLE_OP_WRITER_U32(call, CALL);
	DEFINE_SIMPLE_OP_WRITER_U32(get, GET);
	DEFINE_SIMPLE_OP_WRITER_U32(get_local, GET_LOCAL);
	DEFINE_SIMPLE_OP_WRITER_U32(set, SET);
	DEFINE_SIMPLE_OP_WRITER_U32(syscall, SYSCALL);
	DEFINE_SIMPLE_OP_WRITER_U32(const, CONST32);

#undef DEFINE_SIMPLE_OP_WRITER
#undef DEFINE_SIMPLE_OP_WRITER_U32

void kvm_bc_const_float(kvm_bc_writer* writer, float value) {
	kvm_u32 size = sizeof(KVM_OP) + sizeof(value);
	if (writer->capacity < size) return;
	KVM_OP op = KVM_OP_CONST32;
	memcpy(writer->ip, &op, sizeof(op));
	memcpy(writer->ip + sizeof(op), &value, sizeof(value));
	writer->ip += size;
}

void kvm_bc_const64(kvm_bc_writer* writer, kvm_u64 value) {
	kvm_u32 size = sizeof(KVM_OP) + sizeof(value);
	if (writer->capacity < size) return;
	KVM_OP op = KVM_OP_CONST64;
	memcpy(writer->ip, &op, sizeof(op));
	memcpy(writer->ip + sizeof(op), &value, sizeof(value));
	writer->ip += size;
}


kvm_label kvm_bc_create_label(kvm_bc_writer* writer) {
	writer->labels[writer->labels_count] = KVM_INVALID_LABEL;
	++writer->labels_count;
	return writer->labels_count - 1;
}

void kvm_bc_place_label(kvm_bc_writer* writer, kvm_label label) {
	writer->labels[label] = (kvm_u32)(writer->ip - writer->bytecode);
}
