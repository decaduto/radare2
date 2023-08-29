#include <r_arch.h>
#include <r_types.h>
#include <r_lib.h>
#include <r_cmd.h>
#include <r_core.h>

#define _GNU_SOURCE
#include <stdio.h>
#include "nds32-opc.h"
#include "nds32-dis.h"

typedef uint32_t insn_t;

#if 0
static CpuKv cpus[] = {
	{ "nds32", nds32 },
	{ NULL, 0 }
};
#endif


#define is_any(...) _is_any(name, __VA_ARGS__, NULL)
static bool _is_any(const char *str, ...) {
	char *cur;
	va_list va;
	va_start (va, str);
	while (true) {
		cur = va_arg (va, char *);
		if (!cur) {
			break;
		}
		if (r_str_startswith (str, cur)) {
			va_end (va);
			return true;
		}
	}
	va_end (va);
	return false;
}

static int info(RArchSession *as, ut32 q) {
	switch (q) {
	case R_ANAL_ARCHINFO_MAX_OP_SIZE:
		return 6;
	case R_ANAL_ARCHINFO_MIN_OP_SIZE:
		return 2;
	}
	return 0;
}

static inline unsigned int nds32_insn_length(insn_t insn){
	return 4;
}

static struct nds32_opcode *nds32_get_opcode(PluginData *pd, insn_t word) {
	struct nds32_opcode *op = NULL;

#define OP_HASH_IDX(i) ((i) & (nds32_insn_length (i) == 2 ? 3 : OP_MASK_OP))
	if (!pd->init0) {
		size_t i;
		for (i = 0; i < OP_MASK_OP + 1; i++) {
			pd->riscv_hash[i] = 0;
		}
		for (op = nds32_opcodes; op <= &nds32_opcodes[NUMOPCODES - 1]; op++) {
			if (!pd->nds32_hash[OP_HASH_IDX (op->match)]) {
				pd->nds32_hash[OP_HASH_IDX (op->match)] = op;
			}
		}
		pd->init0 = true;
	}
	return (struct nds32_opcode *) pd->nds32_hash[OP_HASH_IDX (word)];
}

static int nds32_buffer_read_memory(bfd_vma memaddr, bfd_byte *myaddr, ut32 length, struct disassemble_info *info) {
	int delta = (memaddr - info->buffer_vma);
	if (delta < 0) {
		return -1; // disable backward reads
	}
	if ((delta + length) > 4) {
		return -1;
	}
	ut8 *bytes = info->buffer;
	memcpy (myaddr, bytes + delta, length);
	return 0;
}

static int symbol_at_address(bfd_vma addr, struct disassemble_info *info) {
	return 0;
}

static void memory_error_func(int status, bfd_vma memaddr, struct disassemble_info *info) {
	//--
}

DECLARE_GENERIC_PRINT_ADDRESS_FUNC_NOGLOBALS()
DECLARE_GENERIC_FPRINTF_FUNC_NOGLOBALS()

static bool decode(RArchSession *as, RAnalOp *op, RAnalOpMask mask) {
	const ut64 addr = op->addr;
	const int len = op->size;
	const ut8 *buf = op->bytes;
	ut8 bytes[8] = {0};
	insn_t word = {0};
	struct disassemble_info disasm_obj = {0};
	RStrBuf *sb = NULL;
	if (mask & R_ARCH_OP_MASK_DISASM) {
		sb = r_strbuf_new (NULL);
	}
	memcpy (bytes, buf, R_MIN (sizeof (bytes), len)); // TODO handle thumb
	/* prepare disassembler */
	disasm_obj.buffer = bytes;
	disasm_obj.buffer_vma = addr;
	disasm_obj.read_memory_func = &nds32_buffer_read_memory;
	disasm_obj.symbol_at_address_func = &symbol_at_address;
	disasm_obj.memory_error_func = &memory_error_func;
	disasm_obj.print_address_func = &generic_print_address_func;
	disasm_obj.endian = !R_ARCH_CONFIG_IS_BIG_ENDIAN (as->config);
	disasm_obj.fprintf_func = &generic_fprintf_func;
	disasm_obj.stream = sb;
	disasm_obj.mach = 0; // TODO: detect_cpu (as->config->cpu);
	op->size = print_insn_nds32((bfd_vma)addr, &disasm_obj);

	if (mask & R_ARCH_OP_MASK_DISASM) {
		op->mnemonic = r_strbuf_drain (sb);
		sb = NULL;
		r_str_replace_ch (op->mnemonic, '\t', ' ', true);
	}
	int left = R_MIN (len, op->size);
	if (left < 1 || (left > 0 && !memcmp (buf, "\xff\xff\xff\xff\xff\xff\xff\xff", left))) {
		op->mnemonic = strdup ("breakpoint");
		r_strbuf_free (sb);
		return true;
	}
	PluginData *pd = s->data;
	struct nds32_opcode *o = nds32_get_opcode(pd, word);
	const char *name = o->instruction;
	if (op->mnemonic) {
		name = op->mnemonic;
	}

	const char *arg = strstr (name, "0x");
	if (!arg) {
		arg = strstr (name, ", ");
		if (arg) {
			arg++;
		} else {
			arg = strchr (name, ' ');
			if (arg) {
				arg++;
			}
		}
	}
	if( is_any("jal ", "jral ", "j ") ){
		// decide whether it's jump or call
		#ifndef OP_MASK_RD // riscv arch dependent, not nds32 code
			#define OP_MASK_RD		0x1f
			#define OP_SH_RD		11
		#endif
		int rd = (word >> OP_SH_RD) & OP_MASK_RD;
		op->type = (rd == 0) ? R_ANAL_OP_TYPE_JMP: R_ANAL_OP_TYPE_CALL;
		// op->jump = EXTRACT_UJTYPE_IMM (word) + addr;
		op->jump = arg? r_num_get (NULL, arg): op->addr;
		if (op->type == R_ANAL_OP_TYPE_CALL) {
			op->fail = addr + op->size;
		}
	}
	if( is_any("jr ") ){
		op->type = R_ANAL_OP_TYPE_RJMP;
	}
	if( is_any("ret ") ){
		op->type = R_ANAL_OP_TYPE_RET;
	}
	if( is_any("bgezal ", "bltzal ") ){

	}
	if (is_any ("beqz", "beq", "blez", "bgez", "ble", "bltz", "bgtz", "bnez", "bne ")) {
		op->type = R_ANAL_OP_TYPE_CJMP;
		// op->jump = EXTRACT_SBTYPE_IMM (word) + addr;
		op->jump = arg? r_num_get (NULL, arg): op->addr;
		op->fail = addr + op->size;
	}
	r_strbuf_free (sb);
	return op->size > 0;
}

const RArchPlugin r_arch_plugin_nds32 = {
	.meta = {
		.name = "nds32",
		.author = "Edoardo Mantovani",
		.license = "GPL3",
		.desc = "Binutils based nds32 disassembler",
	},
	.arch = "nds32",
	.bits = R_SYS_BITS_PACK1 (32),
	.endian = R_SYS_ENDIAN_LITTLE,
	.decode = &decode,
	.info = &info,
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_ARCH,
	.data = &r_arch_plugin_nds32,
};
#endif
