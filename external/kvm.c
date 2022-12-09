#include "kvm.h"
#include <stdbool.h>
#include <string.h>

#ifdef _MSC_VER
	#pragma warning(error : 4062)
#endif

typedef union kvm_reg32 {
	kvm_u32 u;
	kvm_i32 i;
	float f;
} kvm_reg32;

typedef enum KVM_OP {
	// end execution
	KVM_OP_END,
	
	// call syscall with fixed number of args, pop the args after
	KVM_OP_SYSCALL,

	// push 32bit constant to stack, and store it in eax
	KVM_OP_CONST32,

	// push 64bit constant to stack, and store it in eax
	KVM_OP_CONST64,

	// push eax to stack
	KVM_OP_PUSH,

	// pop from stack to eax
	KVM_OP_POP,

	// jump if eax == ebx
	KVM_OP_EQ,

	// eax += ebx
	KVM_OP_ADD,
	
	// eax += ebx (as floats)
	KVM_OP_ADDF,
	
	// eax *= ebx
	KVM_OP_MUL,
	
	// eax *= ebx
	KVM_OP_MULF,
	
	// jump to eax
	KVM_OP_JMP,
	
	// pop return address and jump to it
	KVM_OP_RET,
	
	// push return address and jump
	KVM_OP_CALL,
	
	// eax = env[idx], push(eax)
	KVM_OP_GET,
	
	// env[idx] = eax
	KVM_OP_SET,
} KVM_OP;

kvm_u32 kvm_get(KVM* vm, kvm_i32 idx) {
	kvm_u32 res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx * sizeof(kvm_u32), sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx * sizeof(kvm_u32), sizeof(res));
	}
	return res;
}

void* kvm_get_ptr(KVM* vm, kvm_i32 idx) {
	void *res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx * sizeof(kvm_u32), sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx * sizeof(kvm_u32), sizeof(res));
	}
	return res;

}

kvm_u64 kvm_get64(KVM* vm, kvm_i32 idx) {
	kvm_u64 res;
	if (idx >= 0) {
		memcpy(&res, vm->environment + idx * sizeof(kvm_u32), sizeof(res));
	}
	else {
		memcpy(&res, vm->stack + vm->sp + idx * sizeof(kvm_u32), sizeof(res));
	}
	return res;
}

float kvm_get_float(KVM* vm, kvm_i32 idx) {
	float res;
	memcpy(&res, vm->stack + vm->sp + idx * sizeof(kvm_u32), sizeof(res));
	return res;
}


void kvm_push(KVM* vm, kvm_u32 value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	vm->sp += sizeof(value);
}

void kvm_push_float(KVM* vm, float value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	vm->sp += sizeof(value);
}

void kvm_push_ptr(KVM* vm, void* value) {
	memcpy(vm->stack + vm->sp, &value, sizeof(value));
	vm->sp += sizeof(value);
}

void kvm_init(KVM* vm, kvm_u8* environment, kvm_u32 environment_size) {
	vm->sp = 0;
	vm->environment = environment;
	vm->environment_size = environment_size;
}

void kvm_call(KVM* vm, const kvm_u8* bytecode, kvm_syscall syscall, kvm_label label) {
	
	#define READ(v) \
		do { \
			memcpy(&(v), ip, sizeof(v)); \
			ip += sizeof(v); \
		} while(false)

	const kvm_u8* ip = bytecode + label; 
	kvm_reg32 eax;
	kvm_reg32 ebx;
	eax.u = ebx.u = 0xCCccCCcc;
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
				ebx = eax; 
				READ(eax);
				memcpy(vm->stack + sp, &eax, sizeof(eax));
				sp += sizeof(eax);
				break;
			case KVM_OP_CONST64: {
				kvm_u64 v;
				READ(v);
				memcpy(vm->stack + sp, &v, sizeof(v));
				sp += sizeof(v);
				break;
			}
			case KVM_OP_RET: {
				kvm_u32 addr;
				sp -= sizeof(addr);
				// pop return address
				memcpy(&addr, vm->stack + sp, sizeof(addr));
				ip = bytecode + addr;
				break;
			}
			case KVM_OP_POP:
				ebx = eax;
				sp -= sizeof(eax);
				memcpy(&eax, vm->stack + sp, sizeof(eax));
				break;
			case KVM_OP_CALL: {
				kvm_u32 func_addr;
				READ(func_addr);

				kvm_u32 ret_addr = (kvm_u32)(ip - bytecode);
				memcpy(vm->stack + sp, &ret_addr, sizeof(ret_addr));
				sp += sizeof(ret_addr);
				ip = bytecode + func_addr;
				break;
			}
			case KVM_OP_SET: {
				kvm_i32 idx;
				READ(idx);
				memcpy(vm->environment + idx * sizeof(kvm_u32), &eax, sizeof(eax));
				break;
			}
			case KVM_OP_GET: {
				kvm_i32 idx;
				READ(idx);
				memcpy(&eax, vm->environment + idx * sizeof(kvm_u32), sizeof(eax));
				memcpy(vm->stack + sp, &eax, sizeof(eax));
				sp += sizeof(eax);
				break;
			}
			case KVM_OP_JMP: 
				ip = bytecode + eax.u;
				break;
			case KVM_OP_MUL:
				eax.u = eax.u * ebx.u;
				break;
			case KVM_OP_MULF:
				eax.f = eax.f * ebx.f;
				break;
			case KVM_OP_ADD:
				eax.u = eax.u + ebx.u;
				break;
			case KVM_OP_ADDF:
				eax.f = eax.f + ebx.f;
				break;
			case KVM_OP_EQ: {
				kvm_u32 jump_address;
				READ(jump_address);
				if (eax.u == ebx.u) {
					ip = bytecode + jump_address;
				}
				break;
			}
			case KVM_OP_PUSH:
				memcpy(vm->stack + sp, &eax, sizeof(eax));
				sp += sizeof(eax);
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
			case KVM_OP_PUSH:
			case KVM_OP_JMP:
			case KVM_OP_MUL:
			case KVM_OP_MULF:
			case KVM_OP_ADD:
			case KVM_OP_ADDF:
			case KVM_OP_END:
				break;
			case KVM_OP_EQ:
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
	DEFINE_SIMPLE_OP_WRITER(push, PUSH);
	DEFINE_SIMPLE_OP_WRITER(pop, POP);
	DEFINE_SIMPLE_OP_WRITER(add, ADD);
	DEFINE_SIMPLE_OP_WRITER(addf, ADDF);
	DEFINE_SIMPLE_OP_WRITER(mul, MUL);
	DEFINE_SIMPLE_OP_WRITER(mulf, MULF);
	DEFINE_SIMPLE_OP_WRITER(jmp, JMP);
	DEFINE_SIMPLE_OP_WRITER(ret, RET);

	DEFINE_SIMPLE_OP_WRITER_U32(get, GET);
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

void kvm_bc_eq(kvm_bc_writer* writer, kvm_label label) {
	kvm_u32 size = sizeof(KVM_OP) + sizeof(kvm_label);
	if (writer->capacity < size) return;
	KVM_OP op = KVM_OP_EQ;
	memcpy(writer->ip, &op, sizeof(op));
	memcpy(writer->ip + sizeof(op), &label, sizeof(label));
	writer->ip += size;
}

void kvm_bc_call(kvm_bc_writer* writer, kvm_label function) {
	kvm_u32 size = sizeof(KVM_OP) + sizeof(function);
	if (writer->capacity < size) return;
	KVM_OP op = KVM_OP_CALL;
	memcpy(writer->ip, &op, sizeof(op));
	memcpy(writer->ip + sizeof(op), &function, sizeof(function));
	writer->ip += size;
}

kvm_label kvm_bc_create_label(kvm_bc_writer* writer) {
	writer->labels[writer->labels_count] = 0xFFffFFff;
	++writer->labels_count;
	return writer->labels_count - 1;
}

void kvm_bc_place_label(kvm_bc_writer* writer, kvm_label label) {
	writer->labels[label] = (kvm_u32)(writer->ip - writer->bytecode);
}
