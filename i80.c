/*
 * Copyright (c) 2020 Brian Callahan <bcallah@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <fcntl.h>
#include <unistd.h>

#define AC_ADD	0
#define AC_SUB	1

typedef uint8_t		byte;
typedef uint16_t	word;

static byte ram[0x10000];	/* +1 for terminating NUL-byte */
static byte inout[256];

static int port = -1;

struct cpu {
	byte a;
	byte b;
	byte c;
	byte d;
	byte e;
	byte h;
	byte l;

	byte f;

	byte fs;
	byte fz;
	byte fzero;
	byte fac;
	byte fzerox;
	byte fp;
	byte fone;
	byte fcy;

	word sp;
	word pc;

	byte inte;
};

static void
ret(struct cpu *i80)
{

	i80->pc = ram[i80->sp++];
	i80->pc |= ram[i80->sp++] << 8;
}

static void
call(struct cpu *i80)
{

	ram[--i80->sp] = (i80->pc) >> 8;
	ram[--i80->sp] = (i80->pc) & 0xff;
}

static byte
parity(byte a)
{
	byte p = 0;

	while (a) {
		p = !p;
		a = a & (a - 1);
	}

	return !p;
}

static void
flags(struct cpu *i80, byte reg)
{

	if (reg > 0x7f)
		i80->fs = 1;
	else
		i80->fs = 0;

	if (reg == 0)
		i80->fz = 1;
	else
		i80->fz = 0;

	i80->fp = parity(reg);

	i80->fzero = 0;
	i80->fzerox = 0;
	i80->fone = 1;
}

/*
 * AC flag miscalculated?
 */
static void
carryflag(struct cpu *i80, byte rega, byte regb, word sum, int addsub)
{
	word halfcarry;

	if (addsub == AC_ADD)
		halfcarry = (rega & 0xf) + (regb & 0xf) + i80->fcy;
	else
		halfcarry = (rega & 0xf) + (regb & 0xf) + 1 - i80->fcy;

	if (halfcarry > 0xf)
		i80->fac = 1;
	else
		i80->fac = 0;

	if (addsub == AC_SUB)
		i80->fac = !i80->fac;

	if (sum > 0xff)
		i80->fcy = 1;
	else
		i80->fcy = 0;
}

static void
daa(struct cpu *i80)
{
	word carry;

	if ((((i80->a) & 0xf) > 9) || i80->fac == 1) {
		if ((((i80->a) & 0xf) + 0x6) > 0xf)
			i80->fac = 1;
		else
			i80->fac = 0;

		i80->a += 0x6;
	}

	if ((((i80->a) >> 4) > 9) || i80->fcy == 1) {
		carry = i80->a + 0x60;
		if (carry > 0xff)
			i80->fcy = 1;

		i80->a += 0x60;
	}

	flags(i80, i80->a);
}

static int
execute(struct cpu *i80, byte opcode)
{
	uint32_t doublecarry;
	word carry = 0, sb1, sb2;
	byte halfcarry = 0;

	switch (opcode) {
	case 0x00:	/* nop */
	case 0x08:
	case 0x10:
	case 0x18:
	case 0x20:
	case 0x28:
	case 0x30:
	case 0x38:
		break;
	case 0x01:	/* lxi b, i16 */
		i80->c = ram[i80->pc++];
		i80->b = ram[i80->pc++];
		break;
	case 0x02:	/* stax b */
		ram[((i80->b) << 8) | i80->c] = i80->a;
		break;
	case 0x03:	/* inx b */
		i80->c++;
		if (i80->c == 0)	/* overflow */
			i80->b++;
		break;
	case 0x04:	/* inr b */
		i80->b++;
		if ((i80->b & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->b);
		break;
	case 0x05:	/* dcr b */
		i80->b--;
		if ((i80->b & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->b);
		break;
	case 0x06:	/* mvi b, i8 */
		i80->b = ram[i80->pc++];
		break;
	case 0x07:	/* rlc */
		carry = (i80->a) << 1;
		if (carry > 0xff)
			i80->fcy = 1;
		else
			i80->fcy = 0;
		i80->a = carry & 0xff;
		if (i80->fcy == 1)
			i80->a++;
		break;
	case 0x09:	/* dad b */
		sb1 = ((i80->b) << 8) | i80->c;
		sb2 = ((i80->h) << 8) | i80->l;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			i80->fcy = 1;
		else
			i80->fcy = 0;

		i80->h = (doublecarry >> 8) & 0xff;
		i80->l = doublecarry & 0xff;
		break;
	case 0x0a:	/* ldax b */
		i80->a = ram[((i80->b) << 8) | i80->c];
		break;
	case 0x0b:	/* dcx b */
		i80->c--;
		if (i80->c == 0xff)	/* underflow */
			i80->b--;
		break;
	case 0x0c:	/* inr c */
		i80->c++;
		if ((i80->c & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->c);
		break;
	case 0x0d:	/* dcr c */
		i80->c--;
		if ((i80->c & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->c);
		break;
	case 0x0e:	/* mvi c, i8 */
		i80->c = ram[i80->pc++];
		break;
	case 0x0f:	/* rrc */
		carry = (i80->a) & 0x1;
		i80->a = (i80->a) >> 1;
		if (carry) {
			i80->a += 0x80;
			i80->fcy = 1;
		} else {
			i80->fcy = 0;
		}
		break;
	case 0x11:	/* lxi d, i16 */
		i80->e = ram[i80->pc++];
		i80->d = ram[i80->pc++];
		break;
	case 0x12:	/* stax d */
		ram[((i80->d) << 8) | i80->e] = i80->a;
		break;
	case 0x13:	/* inx d */
		i80->e++;
		if (i80->e == 0)	/* overflow */
			i80->d++;
		break;
	case 0x14:	/* inr d */
		i80->d++;
		if ((i80->d & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->d);
		break;
	case 0x15:	/* dcr d */
		i80->d--;
		if ((i80->d & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->d);
		break;
	case 0x16:	/* mvi d, i8 */
		i80->d = ram[i80->pc++];
		break;
	case 0x17:	/* ral */
		carry = (i80->a) << 1;
		i80->a = carry & 0xff;
		if (i80->fcy == 1)
			i80->a++;

		if (carry > 0xff)
			i80->fcy = 1;
		else
			i80->fcy = 0;
		break;
	case 0x19:	/* dad d */
		sb1 = ((i80->d) << 8) | i80->e;
		sb2 = ((i80->h) << 8) | i80->l;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			i80->fcy = 1;
		else
			i80->fcy = 0;

		i80->h = (doublecarry >> 8) & 0xff;
		i80->l = doublecarry & 0xff;
		break;
	case 0x1a:	/* ldax d */
		i80->a = ram[((i80->d) << 8) | i80->e];
		break;
	case 0x1b:	/* dcx d */
		i80->e--;
		if (i80->e == 0xff)	/* underflow */
			i80->d--;
		break;
	case 0x1c:	/* inr e */
		i80->e++;
		if ((i80->e & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->e);
		break;
	case 0x1d:	/* dcr e */
		i80->e--;
		if ((i80->e & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->e);
		break;
	case 0x1e:	/* mvi e, i8 */
		i80->e = ram[i80->pc++];
		break;
	case 0x1f:	/* rar */
		carry = (i80->a) & 0x1;
		i80->a = (i80->a) >> 1;
		if (i80->fcy == 1)
			i80->a += 0x80;

		if (carry)
			i80->fcy = 1;
		else
			i80->fcy = 0;
		break;
	case 0x21:	/* lxi h, i16 */
		i80->l = ram[i80->pc++];
		i80->h = ram[i80->pc++];
		break;
	case 0x22:	/* shld i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		ram[sb1++] = i80->l;
		ram[sb1] = i80->h;
		break;
	case 0x23:	/* inx h */
		i80->l++;
		if (i80->l == 0)	/* overflow */
			i80->h++;
		break;
	case 0x24:	/* inr h */
		i80->h++;
		if ((i80->h & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->h);
		break;
	case 0x25:	/* dcr h */
		i80->h--;
		if ((i80->h & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->h);
		break;
	case 0x26:	/* mvi h, i8 */
		i80->h = ram[i80->pc++];
		break;
	case 0x27:	/* daa */
		daa(i80);
		break;
	case 0x29:	/* dad h */
		sb1 = ((i80->h) << 8) | i80->l;
		sb2 = sb1;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			i80->fcy = 1;
		else
			i80->fcy = 0;

		i80->h = (doublecarry >> 8) & 0xff;
		i80->l = doublecarry & 0xff;
		break;
	case 0x2a:	/* lhld i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		i80->l = ram[sb1++];
		i80->h = ram[sb1];
		break;
	case 0x2b:	/* dcx h */
		i80->l--;
		if (i80->l == 0xff)	/* underflow */
			i80->h--;
		break;
	case 0x2c:	/* inr l */
		i80->l++;
		if ((i80->l & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->l);
		break;
	case 0x2d:	/* dcr l */
		i80->l--;
		if ((i80->l & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->l);
		break;
	case 0x2e:	/* mvi l, i8 */
		i80->l = ram[i80->pc++];
		break;
	case 0x2f:	/* cma */
		i80->a = ~(i80->a);
		break;
	case 0x31:	/* lxi sp, i16 */
		i80->sp = ram[i80->pc++];
		i80->sp |= ram[i80->pc++] << 8;
		break;
	case 0x32:	/* sta i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		ram[sb1] = i80->a;
		break;
	case 0x33:	/* inx sp */
		++i80->sp;
		break;
	case 0x34:	/* inr m */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1]++;
		if ((ram[sb1] & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, ram[sb1]);
		break;
	case 0x35:	/* dcr m */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1]--;
		if ((ram[sb1] & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, ram[sb1]);
		break;
	case 0x36:	/* mvi m, i8 */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = ram[i80->pc++];
		break;
	case 0x37:	/* stc */
		i80->fcy = 1;
		break;
	case 0x39:	/* dad sp */
		sb1 = ((i80->h) << 8) | i80->l;
		doublecarry = i80->sp + sb1;

		if (doublecarry > 0xffff)
			i80->fcy = 1;
		else
			i80->fcy = 0;

		i80->h = (doublecarry >> 8) & 0xff;
		i80->l = doublecarry & 0xff;
		break;
	case 0x3a:	/* lda i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		i80->a = ram[sb1];
		break;
	case 0x3b:	/* dcx sp */
		--i80->sp;
		break;
	case 0x3c:	/* inr a */
		i80->a++;
		if ((i80->a & 0xf) == 0)
			i80->fac = 1;
		else
			i80->fac = 0;

		flags(i80, i80->a);
		break;
	case 0x3d:	/* dcr a */
		i80->a--;
		if ((i80->a & 0xf) == 0xf)
			i80->fac = 0;
		else
			i80->fac = 1;

		flags(i80, i80->a);
		break;
	case 0x3e:	/* mvi a, i8 */
		i80->a = ram[i80->pc++];
		break;
	case 0x3f:	/* cmc */
		if (i80->fcy == 1)
			i80->fcy = 0;
		else
			i80->fcy = 1;
		break;
	case 0x40:	/* mov b, b */
		/* cheat, do nothing */
		break;
	case 0x41:	/* mov b, c */
		i80->b = i80->c;
		break;
	case 0x42:	/* mov b, d */
		i80->b = i80->d;
		break;
	case 0x43:	/* mov b, e */
		i80->b = i80->e;
		break;
	case 0x44:	/* mov b, h */
		i80->b = i80->h;
		break;
	case 0x45:	/* mov b, l */
		i80->b = i80->l;
		break;
	case 0x46:	/* mov b, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->b = ram[sb1];
		break;
	case 0x47:	/* mov b, a */
		i80->b = i80->a;
		break;
	case 0x48:	/* mov c, b */
		i80->c = i80->b;
		break;
	case 0x49:	/* mov c, c */
		/* cheat, do nothing */
		break;
	case 0x4a:	/* mov c, d */
		i80->c = i80->d;
		break;
	case 0x4b:	/* mov c, e */
		i80->c = i80->e;
		break;
	case 0x4c:	/* mov c, h */
		i80->c = i80->h;
		break;
	case 0x4d:	/* mov c, l */
		i80->c = i80->l;
		break;
	case 0x4e:	/* mov c, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->c = ram[sb1];
		break;
	case 0x4f:	/* mov c, a */
		i80->c = i80->a;
		break;
	case 0x50:	/* mov d, b */
		i80->d = i80->b;
		break;
	case 0x51:	/* mov d, c */
		i80->d = i80->c;
		break;
	case 0x52:	/* mov d, d */
		/* cheat, do nothing */
		break;
	case 0x53:	/* mov d, e */
		i80->d = i80->e;
		break;
	case 0x54:	/* mov d, h */
		i80->d = i80->h;
		break;
	case 0x55:	/* mov d, l */
		i80->d = i80->l;
		break;
	case 0x56:	/* mov d, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->d = ram[sb1];
		break;
	case 0x57:	/* mov d, a */
		i80->d = i80->a;
		break;
	case 0x58:	/* mov e, b */
		i80->e = i80->b;
		break;
	case 0x59:	/* mov e, c */
		i80->e = i80->c;
		break;
	case 0x5a:	/* mov e, d */
		i80->e = i80->d;
		break;
	case 0x5b:	/* mov e, e */
		/* cheat, do nothing */
		break;
	case 0x5c:	/* mov e, h */
		i80->e = i80->h;
		break;
	case 0x5d:	/* mov e, l */
		i80->e = i80->l;
		break;
	case 0x5e:	/* mov e, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->e = ram[sb1];
		break;
	case 0x5f:	/* mov e, a */
		i80->e = i80->a;
		break;
	case 0x60:	/* mov h, b */
		i80->h = i80->b;
		break;
	case 0x61:	/* mov h, c */
		i80->h = i80->c;
		break;
	case 0x62:	/* mov h, d */
		i80->h = i80->d;
		break;
	case 0x63:	/* mov h, e */
		i80->h = i80->e;
		break;
	case 0x64:	/* mov h, h */
		/* cheat, do nothing */
		break;
	case 0x65:	/* mov h, l */
		i80->h = i80->l;
		break;
	case 0x66:	/* mov h, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->h = ram[sb1];
		break;
	case 0x67:	/* mov h, a */
		i80->h = i80->a;
		break;
	case 0x68:	/* mov l, b */
		i80->l = i80->b;
		break;
	case 0x69:	/* mov l, c */
		i80->l = i80->c;
		break;
	case 0x6a:	/* mov l, d */
		i80->l = i80->d;
		break;
	case 0x6b:	/* mov l, e */
		i80->l = i80->e;
		break;
	case 0x6c:	/* mov l, h */
		i80->l = i80->h;
		break;
	case 0x6d:	/* mov l, l */
		/* cheat, do nothing */
		break;
	case 0x6e:	/* mov l, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->l = ram[sb1];
		break;
	case 0x6f:	/* mov l, a */
		i80->l = i80->a;
		break;
	case 0x70:	/* mov m, b */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->b;
		break;
	case 0x71:	/* mov m, c */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->c;
		break;
	case 0x72:	/* mov m, d */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->d;
		break;
	case 0x73:	/* mov m, e */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->e;
		break;
	case 0x74:	/* mov m, h */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->h;
		break;
	case 0x75:	/* mov m, l */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->l;
		break;
	case 0x76:	/* hlt */
		return 0;
	case 0x77:	/* mov m, a */
		sb1 = ((i80->h) << 8) | i80->l;
		ram[sb1] = i80->a;
		break;
	case 0x78:	/* mov a, b */
		i80->a = i80->b;
		break;
	case 0x79:	/* mov a, c */
		i80->a = i80->c;
		break;
	case 0x7a:	/* mov a, d */
		i80->a = i80->d;
		break;
	case 0x7b:	/* mov a, e */
		i80->a = i80->e;
		break;
	case 0x7c:	/* mov a, h */
		i80->a = i80->h;
		break;
	case 0x7d:	/* mov a, l */
		i80->a = i80->l;
		break;
	case 0x7e:	/* mov a, m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->a = ram[sb1];
		break;
	case 0x7f:	/* mov a, a */
		/* cheat, do nothing */
		break;
	case 0x80:	/* add b */
		carry = i80->a + i80->b;
		carryflag(i80, i80->a, i80->b, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x81:	/* add c */
		carry = i80->a + i80->c;
		carryflag(i80, i80->a, i80->c, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x82:	/* add d */
		carry = i80->a + i80->d;
		carryflag(i80, i80->a, i80->d, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x83:	/* add e */
		carry = i80->a + i80->e;
		carryflag(i80, i80->a, i80->e, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x84:	/* add h */
		carry = i80->a + i80->h;
		carryflag(i80, i80->a, i80->h, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x85:	/* add l */
		carry = i80->a + i80->l;
		carryflag(i80, i80->a, i80->l, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x86:	/* add m */
		sb1 = ((i80->h) << 8) | i80->l;
		carry = i80->a + ram[sb1];
		carryflag(i80, i80->a, ram[sb1], carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x87:	/* add a */
		carry = i80->a + i80->a;
		carryflag(i80, i80->a, i80->a, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x88:	/* adc b */
		carry = i80->a + i80->b + i80->fcy;
		carryflag(i80, i80->a, i80->b, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x89:	/* adc c */
		carry = i80->a + i80->c + i80->fcy;
		carryflag(i80, i80->a, i80->c, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8a:	/* adc d */
		carry = i80->a + i80->d + i80->fcy;
		carryflag(i80, i80->a, i80->d, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8b:	/* adc e */
		carry = i80->a + i80->e + i80->fcy;
		carryflag(i80, i80->a, i80->e, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8c:	/* adc h */
		carry = i80->a + i80->h + i80->fcy;
		carryflag(i80, i80->a, i80->h, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8d:	/* adc l */
		carry = i80->a + i80->l + i80->fcy;
		carryflag(i80, i80->a, i80->l, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8e:	/* adc m */
		sb1 = ((i80->h) << 8) | i80->l;
		carry = i80->a + ram[sb1] + i80->fcy;
		carryflag(i80, i80->a, ram[sb1], carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x8f:	/* adc a */
		carry = i80->a + i80->a + i80->fcy;
		carryflag(i80, i80->a, i80->a, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x90:	/* sub b */
		carry = i80->a + ~(i80->b) + 1;
		carryflag(i80, i80->a, ~(i80->b), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x91:	/* sub c */
		carry = i80->a + ~(i80->c) + 1;
		carryflag(i80, i80->a, ~(i80->c), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x92:	/* sub d */
		carry = i80->a + ~(i80->d) + 1;
		carryflag(i80, i80->a, ~(i80->d), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x93:	/* sub e */
		carry = i80->a + ~(i80->e) + 1;
		carryflag(i80, i80->a, ~(i80->e), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x94:	/* sub h */
		carry = i80->a + ~(i80->h) + 1;
		carryflag(i80, i80->a, ~(i80->h), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x95:	/* sub l */
		carry = i80->a + ~(i80->l) + 1;
		carryflag(i80, i80->a, ~(i80->l), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x96:	/* sub m */
		sb1 = ((i80->h) << 8) | i80->l;
		carry = i80->a + ~(ram[sb1]) + 1;
		carryflag(i80, i80->a, ~(ram[sb1]), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x97:	/* sub a */
		carry = i80->a + ~(i80->a) + 1;
		carryflag(i80, i80->a, ~(i80->a), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x98:	/* sbb b */
		carry = i80->a + ~(i80->b) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->b), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x99:	/* sbb c */
		carry = i80->a + ~(i80->c) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->c), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9a:	/* sbb d */
		carry = i80->a + ~(i80->d) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->d), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9b:	/* sbb e */
		carry = i80->a + ~(i80->e) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->e), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9c:	/* sbb h */
		carry = i80->a + ~(i80->h) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->h), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9d:	/* sbb l */
		carry = i80->a + ~(i80->l) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->l), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9e:	/* sbb m */
		sb1 = ((i80->h) << 8) | i80->l;
		carry = i80->a + ~(ram[sb1]) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(ram[sb1]), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0x9f:	/* sbb a */
		carry = i80->a + ~(i80->a) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(i80->a), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0xa0:	/* ana b */
		i80->a = i80->a & i80->b;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa1:	/* ana c */
		i80->a = i80->a & i80->c;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa2:	/* ana d */
		i80->a = i80->a & i80->d;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa3:	/* ana e */
		i80->a = i80->a & i80->e;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa4:	/* ana h */
		i80->a = i80->a & i80->h;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa5:	/* ana l */
		i80->a = i80->a & i80->l;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa6:	/* ana m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->a = i80->a & ram[sb1];
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa7:	/* ana a */
		i80->a = i80->a & i80->a;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa8:	/* xra b */
		i80->a = i80->a ^ i80->b;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xa9:	/* xra c */
		i80->a = i80->a ^ i80->c;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xaa:	/* xra d */
		i80->a = i80->a ^ i80->d;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xab:	/* xra e */
		i80->a = i80->a ^ i80->e;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xac:	/* xra h */
		i80->a = i80->a ^ i80->h;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xad:	/* xra l */
		i80->a = i80->a ^ i80->l;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xae:	/* xra m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->a = i80->a ^ ram[sb1];
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xaf:	/* xra a */
		i80->a = i80->a ^ i80->a;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb0:	/* ora b */
		i80->a = i80->a | i80->b;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb1:	/* ora c */
		i80->a = i80->a | i80->c;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb2:	/* ora d */
		i80->a = i80->a | i80->d;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb3:	/* ora e */
		i80->a = i80->a | i80->e;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb4:	/* ora h */
		i80->a = i80->a | i80->h;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb5:	/* ora l */
		i80->a = i80->a | i80->l;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb6:	/* ora m */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->a = i80->a | ram[sb1];
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb7:	/* ora a */
		i80->a = i80->a | i80->a;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xb8:	/* cmp b */
		carry = i80->a + ~(i80->b) + 1;
		carryflag(i80, i80->a, ~(i80->b), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xb9:	/* cmp c */
		carry = i80->a + ~(i80->c) + 1;
		carryflag(i80, i80->a, ~(i80->c), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xba:	/* cmp d */
		carry = i80->a + ~(i80->d) + 1;
		carryflag(i80, i80->a, ~(i80->d), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xbb:	/* cmp e */
		carry = i80->a + ~(i80->e) + 1;
		carryflag(i80, i80->a, ~(i80->e), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xbc:	/* cmp h */
		carry = i80->a + ~(i80->h) + 1;
		carryflag(i80, i80->a, ~(i80->h), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xbd:	/* cmp l */
		carry = i80->a + ~(i80->l) + 1;
		carryflag(i80, i80->a, ~(i80->l), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xbe:	/* cmp m */
		sb1 = ((i80->h) << 8) | i80->l;
		carry = i80->a + ~(ram[sb1]) + 1;
		carryflag(i80, i80->a, ~(ram[sb1]), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xbf:	/* cmp a */
		carry = i80->a + ~(i80->a) + 1;
		carryflag(i80, i80->a, ~(i80->a), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xc0:	/* rnz */
		if (i80->fz == 0)
			ret(i80);
		break;
	case 0xc1:	/* pop b */
		i80->c = ram[i80->sp++];
		i80->b = ram[i80->sp++];
		break;
	case 0xc2:	/* jnz i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fz == 0)
			i80->pc = sb1;
		break;
	case 0xc3:	/* jmp i16 */
	case 0xcb:
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		i80->pc = sb1;
		break;
	case 0xc4:	/* cnz i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fz == 0) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xc5:	/* push b */
		ram[--i80->sp] = i80->b;
		ram[--i80->sp] = i80->c;
		break;
	case 0xc6:	/* adi i8 */
		halfcarry = ram[i80->pc++];
		carry = i80->a + halfcarry;
		carryflag(i80, i80->a, halfcarry, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0xc7:	/* rst 0 */
		call(i80);
		i80->pc = 0x00;
		break;
	case 0xc8:	/* rz */
		if (i80->fz == 1)
			ret(i80);
		break;
	case 0xc9:	/* ret */
	case 0xd9:
		ret(i80);
		break;
	case 0xca:	/* jz i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fz == 1)
			i80->pc = sb1;
		break;
	case 0xcc:	/* cz i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fz == 1) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xcd:	/* call i16 */
	case 0xdd:
	case 0xed:
	case 0xfd:
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		call(i80);
		i80->pc = sb1;
		break;
	case 0xce:	/* aci i8 */
		halfcarry = ram[i80->pc++];
		carry = i80->a + halfcarry + i80->fcy;
		carryflag(i80, i80->a, halfcarry, carry, AC_ADD);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0xcf:	/* rst 1 */
		call(i80);
		i80->pc = 0x08;
		break;
	case 0xd0:	/* rnc */
		if (i80->fcy == 0)
			ret(i80);
		break;
	case 0xd1:	/* pop d */
		i80->e = ram[i80->sp++];
		i80->d = ram[i80->sp++];
		break;
	case 0xd2:	/* jnc i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fcy == 0)
			i80->pc = sb1;
		break;
	case 0xd3:	/* out i8 */
		port = ram[i80->pc++];
		inout[port] = i80->a;
		break;
	case 0xd4:	/* cnc i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fcy == 0) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xd5:	/* push d */
		ram[--i80->sp] = i80->d;
		ram[--i80->sp] = i80->e;
		break;
	case 0xd6:	/* sui i8 */
		halfcarry = ram[i80->pc++];
		carry = i80->a + ~(halfcarry) + 1;
		carryflag(i80, i80->a, ~(halfcarry), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0xd7:	/* rst 2 */
		call(i80);
		i80->pc = 0x10;
		break;
	case 0xd8:	/* rc */
		if (i80->fcy == 1)
			ret(i80);
		break;
	case 0xda:	/* jc i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fcy == 1)
			i80->pc = sb1;
		break;
	case 0xdb:	/* in i8 */
		port = ram[i80->pc++];
		break;
	case 0xdc:	/* cc i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fcy == 1) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xde:	/* sbi i8 */
		halfcarry = ram[i80->pc++];
		carry = i80->a + ~(halfcarry) + 1 - i80->fcy;
		carryflag(i80, i80->a, ~(halfcarry), carry, AC_SUB);
		i80->a = carry & 0xff;
		flags(i80, i80->a);
		break;
	case 0xdf:	/* rst 3 */
		call(i80);
		i80->pc = 0x18;
		break;
	case 0xe0:	/* rpo */
		if (i80->fp == 0)
			ret(i80);
		break;
	case 0xe1:	/* pop h */
		i80->l = ram[i80->sp++];
		i80->h = ram[i80->sp++];
		break;
	case 0xe2:	/* jpo i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fp == 0)
			i80->pc = sb1;
		break;
	case 0xe3:	/* xthl */
		sb1 = ((i80->h) << 8) | i80->l;
		i80->l = ram[i80->sp];
		i80->h = ram[i80->sp + 1];
		ram[i80->sp] = sb1 & 0xff;
		ram[i80->sp + 1] = (sb1 >> 8) & 0xff;
		break;
	case 0xe4:	/* cpo i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fp == 0) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xe5:	/* push h */
		ram[--i80->sp] = i80->h;
		ram[--i80->sp] = i80->l;
		break;
	case 0xe6:	/* ani i8 */
		i80->a = i80->a & ram[i80->pc++];
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xe7:	/* rst 4 */
		call(i80);
		i80->pc = 0x20;
		break;
	case 0xe8:	/* rpe */
		if (i80->fp == 1)
			ret(i80);
		break;
	case 0xe9:	/* pchl */
		i80->pc = ((i80->h) << 8) | i80->l;
		break;
	case 0xea:	/* jpe i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fp == 1)
			i80->pc = sb1;
		break;
	case 0xeb:	/* xchg */
		sb1 = ((i80->d) << 8) | i80->e;
		i80->d = i80->h;
		i80->e = i80->l;
		i80->h = (sb1 >> 8) & 0xff;
		i80->l = sb1 & 0xff;
		break;
	case 0xec:	/* cpe i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fp == 1) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xee:	/* xri i8 */
		halfcarry = ram[i80->pc++];
		i80->a = i80->a ^ halfcarry;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xef:	/* rst 5 */
		call(i80);
		i80->pc = 0x28;
		break;
	case 0xf0:	/* rp */
		if (i80->fs == 0)
			ret(i80);
		break;
	case 0xf1:	/* pop psw */
		i80->fs = (ram[i80->sp] >> 7) & 0x1;
		i80->fz = (ram[i80->sp] >> 6) & 0x1;
		i80->fzero = 0;
		i80->fac = (ram[i80->sp] >> 4) & 0x1;
		i80->fzerox = 0;
		i80->fp = (ram[i80->sp] >> 2) & 0x1;
		i80->fone = 1;
		i80->fcy = ram[i80->sp++] & 0x1;
		i80->a = ram[i80->sp++];
		break;
	case 0xf2:	/* jp i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fs == 0)
			i80->pc = sb1;
		break;
	case 0xf3:	/* di */
		i80->inte = 0;
		break;
	case 0xf4:	/* cp i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fs == 0) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xf5:	/* push psw */
		ram[--i80->sp] = i80->a;
		ram[--i80->sp] = i80->fs << 7;
		ram[i80->sp] |= i80->fz << 6;
		ram[i80->sp] |= i80->fzero << 5;
		ram[i80->sp] |= i80->fac << 4;
		ram[i80->sp] |= i80->fzerox << 3;
		ram[i80->sp] |= i80->fp << 2;
		ram[i80->sp] |= i80->fone << 1;
		ram[i80->sp] |= i80->fcy;
		break;
	case 0xf6:	/* ori i8 */
		halfcarry = ram[i80->pc++];
		i80->a = i80->a | halfcarry;
		flags(i80, i80->a);
		i80->fac = 0;
		i80->fcy = 0;
		break;
	case 0xf7:	/* rst 6 */
		call(i80);
		i80->pc = 0x30;
		break;
	case 0xf8:	/* rm */
		if (i80->fs == 1)
			ret(i80);
		break;
	case 0xf9:	/* sphl */
		i80->sp = ((i80->h) << 8) | i80->l;
		break;
	case 0xfa:	/* jm i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fs == 1)
			i80->pc = sb1;
		break;
	case 0xfb:	/* ei */
		i80->inte = 1;
		break;
	case 0xfc:	/* cm i16 */
		sb1 = ram[i80->pc++];
		sb1 |= ram[i80->pc++] << 8;
		if (i80->fs == 1) {
			call(i80);
			i80->pc = sb1;
		}
		break;
	case 0xfe:	/* cpi i8 */
		halfcarry = ram[i80->pc++];
		carry = i80->a + ~(halfcarry) + 1;
		carryflag(i80, i80->a, ~(halfcarry), carry, AC_SUB);
		flags(i80, carry);
		break;
	case 0xff:	/* rst 7 */
		call(i80);
		i80->pc = 0x38;
		break;
	}

	return 1;
}

static void
reset(struct cpu *i80)
{

	i80->a = 0;
	i80->b = 0;
	i80->c = 0;
	i80->d = 0;
	i80->e = 0;
	i80->h = 0;
	i80->l = 0;

	i80->fs = 0;
	i80->fz = 1;
	i80->fzero = 0;
	i80->fac = 0;
	i80->fzerox = 0;
	i80->fp = 1;
	i80->fone = 1;
	i80->fcy = 0;

	i80->pc = 0;
	i80->sp = 0;

	i80->inte = 0;
}

/*
 * World's smallest CP/M
 */
static void
cpm(void)
{

	ram[0] = 0x76;

	ram[5] = 0xd3;
	ram[6] = 0x00;
	ram[7] = 0xc9;
}

int
main(int argc, char *argv[])
{
	struct cpu i80;
	int ch, fd, i = 0x100;
	word addr, save, size;

	reset(&i80);
	cpm();

	fd = open(argv[1], O_RDONLY);
	while (i < sizeof(ram) - 1)
		read(fd, &ram[i++], 1);
	close(fd);

	i80.pc = 0x100;
	while (execute(&i80, ram[i80.pc++])) {
		if (port == 0) {
			switch (i80.c) {
			case 0:		/* P_TERMCPM */
				goto out;
			case 1:		/* C_READ */
				while (read(0, &i80.l, 1) < 1)
					;
				i80.a = i80.l;
				write(1, &i80.a, 1);
				break;
			case 2:		/* C_WRITE */
				write(1, &i80.e, 1);
				break;
			case 3:		/* A_READ */
				i80.l = 0;
				i80.a = i80.l;
				break;
			case 4:		/* A_WRITE */
				write(2, &i80.e, 1);
				break;
			case 5:		/* L_WRITE */
				write(2, &i80.e, 1);
				break;
			case 6:		/* C_RAWIO */
				fd = fcntl(0, F_GETFL);
				fcntl(0, F_SETFL, fd | O_NONBLOCK);
				if (read(0, &i80.l, 1) < 1)
					i80.l = 0;
				fcntl(0, F_SETFL, fd & ~(O_NONBLOCK));
				i80.a = i80.l;
				break;
			case 7:		/* Get I/O byte */
				break;
			case 8:		/* Set I/O byte */
				break;
			case 9:		/* C_WRITESTR */
				addr = (i80.d << 8) | i80.e;
				while (ram[addr] != '$')
					write(1, &ram[addr++], 1);
				break;
			case 10:	/* C_READSTR */
				addr = (i80.d << 8) | i80.e;
				size = ram[addr];
				save = addr++;
				++addr;
				while (1) {
					if (read(0, &ch, 1) < 1)
						ch = '\r';

					if (ch == '\n')
						ch = '\r';

					if (ch == '\r')
						break;

					if (addr - save + 2 < size)
						ram[addr] = ch;

					write(1, &ram[addr++], 1);
				}

				addr = addr - save + 1;
				ram[save + 1] = addr & 0xff;

				break;
			case 12:	/* S_BDOSVER */
				i80.h = 0;
				i80.b = i80.h;

				i80.l = 0x22;
				i80.a = i80.l;
				break;
			case 25:	/* DRV_GET */
				i80.a = 0;
			}

			port = -1;
		}
	}

out:
	return 0;
}
