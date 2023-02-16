/*
 * Copyright (c) 2020, 2023 Brian Callahan <bcallah@openbsd.org>
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

struct cpu {
	byte a;
	byte ap;
	byte b;
	byte bp;
	byte c;
	byte cp;
	byte d;
	byte dp;
	byte e;
	byte ep;
	byte h;
	byte hp;
	byte l;
	byte lp;

	byte fs;
	byte fsp;
	byte fz;
	byte fzp;
	byte fzero;
	byte fzerop;
	byte fac;
	byte facp;
	byte fzerox;
	byte fzeroxp;
	byte fp;
	byte fpp;
	byte fone;
	byte fonep;
	byte fcy;
	byte fcyp;

	word sp;
	word pc;

	byte inte;

	byte ram[0xffff];
	byte inout[256];

	int port;
};

static void
ret(struct cpu *z80)
{

	z80->pc = z80->ram[z80->sp++];
	z80->pc |= z80->ram[z80->sp++] << 8;
}

static void
call(struct cpu *z80)
{

	z80->ram[--z80->sp] = (z80->pc) >> 8;
	z80->ram[--z80->sp] = (z80->pc) & 0xff;
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
flags(struct cpu *z80, byte reg)
{

	if (reg > 0x7f)
		z80->fs = 1;
	else
		z80->fs = 0;

	if (reg == 0)
		z80->fz = 1;
	else
		z80->fz = 0;

	z80->fp = parity(reg);

	z80->fzero = 0;
	z80->fzerox = 0;
	z80->fone = 1;
}

/*
 * AC flag miscalculated?
 */
static void
carryflag(struct cpu *z80, byte rega, byte regb, word sum, int addsub)
{
	word halfcarry;

	if (addsub == AC_ADD)
		halfcarry = (rega & 0xf) + (regb & 0xf) + z80->fcy;
	else
		halfcarry = (rega & 0xf) + (regb & 0xf) + 1 - z80->fcy;

	if (halfcarry > 0xf)
		z80->fac = 1;
	else
		z80->fac = 0;

	if (addsub == AC_SUB)
		z80->fac = !z80->fac;

	if (sum > 0xff)
		z80->fcy = 1;
	else
		z80->fcy = 0;
}

static void
daa(struct cpu *z80)
{
	word carry;

	if ((((z80->a) & 0xf) > 9) || z80->fac == 1) {
		if ((((z80->a) & 0xf) + 0x6) > 0xf)
			z80->fac = 1;
		else
			z80->fac = 0;

		z80->a += 0x6;
	}

	if ((((z80->a) >> 4) > 9) || z80->fcy == 1) {
		carry = z80->a + 0x60;
		if (carry > 0xff)
			z80->fcy = 1;

		z80->a += 0x60;
	}

	flags(z80, z80->a);
}

static void
exafaf(struct cpu *z80)
{
	byte ta, tfs, tfz, tfac, tfp, tfcy;

	ta = z80->a;
	tfs = z80->fs;
	tfz = z80->fz;
	tfac = z80->fac;
	tfp = z80->fp;
	tfcy = z80->fcy;

	z80->a = z80->ap;
	z80->fs = z80->fsp;
	z80->fz = z80->fzp;
	z80->fac = z80->facp;
	z80->fp = z80->fpp;
	z80->fcy = z80->fcyp;

	z80->ap = ta;
	z80->fsp = tfs;
	z80->fzp = tfz;
	z80->facp = tfac;
	z80->fpp = tfp;
	z80->fcyp = tfcy;
}

static void
exx(struct cpu *z80)
{
	word tmp;

	tmp = (z80->b << 8) | z80->c;
	z80->b = z80->bp;
	z80->c = z80->cp;
	z80->bp = (tmp >> 8) & 0xff;
	z80->cp = tmp & 0xff;

	tmp = (z80->d << 8) | z80->e;
	z80->d = z80->dp;
	z80->e = z80->ep;
	z80->dp = (tmp >> 8) & 0xff;
	z80->ep = tmp & 0xff;

	tmp = (z80->h << 8) | z80->l;
	z80->h = z80->hp;
	z80->l = z80->lp;
	z80->hp = (tmp >> 8) & 0xff;
	z80->lp = tmp & 0xff;
}

static int
execute(struct cpu *z80, byte opcode)
{
	uint32_t doublecarry;
	word carry = 0, sb1, sb2;
	byte halfcarry = 0;

	switch (opcode) {
	case 0x00:	/* nop */
	case 0x10:
	case 0x18:
	case 0x20:
	case 0x28:
	case 0x30:
	case 0x38:
		break;
	case 0x01:	/* lxi b, i16 */
		z80->c = z80->ram[z80->pc++];
		z80->b = z80->ram[z80->pc++];
		break;
	case 0x02:	/* stax b */
		z80->ram[((z80->b) << 8) | z80->c] = z80->a;
		break;
	case 0x03:	/* inx b */
		z80->c++;
		if (z80->c == 0)	/* overflow */
			z80->b++;
		break;
	case 0x04:	/* inr b */
		z80->b++;
		if ((z80->b & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->b);
		break;
	case 0x05:	/* dcr b */
		z80->b--;
		if ((z80->b & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->b);
		break;
	case 0x06:	/* mvi b, i8 */
		z80->b = z80->ram[z80->pc++];
		break;
	case 0x07:	/* rlc */
		carry = (z80->a) << 1;
		if (carry > 0xff)
			z80->fcy = 1;
		else
			z80->fcy = 0;
		z80->a = carry & 0xff;
		if (z80->fcy == 1)
			z80->a++;
		break;
	case 0x08:	/* ex af, af' */
		exafaf(z80);
		break;
	case 0x09:	/* dad b */
		sb1 = ((z80->b) << 8) | z80->c;
		sb2 = ((z80->h) << 8) | z80->l;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			z80->fcy = 1;
		else
			z80->fcy = 0;

		z80->h = (doublecarry >> 8) & 0xff;
		z80->l = doublecarry & 0xff;
		break;
	case 0x0a:	/* ldax b */
		z80->a = z80->ram[((z80->b) << 8) | z80->c];
		break;
	case 0x0b:	/* dcx b */
		z80->c--;
		if (z80->c == 0xff)	/* underflow */
			z80->b--;
		break;
	case 0x0c:	/* inr c */
		z80->c++;
		if ((z80->c & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->c);
		break;
	case 0x0d:	/* dcr c */
		z80->c--;
		if ((z80->c & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->c);
		break;
	case 0x0e:	/* mvi c, i8 */
		z80->c = z80->ram[z80->pc++];
		break;
	case 0x0f:	/* rrc */
		carry = (z80->a) & 0x1;
		z80->a = (z80->a) >> 1;
		if (carry) {
			z80->a += 0x80;
			z80->fcy = 1;
		} else {
			z80->fcy = 0;
		}
		break;
	case 0x11:	/* lxi d, i16 */
		z80->e = z80->ram[z80->pc++];
		z80->d = z80->ram[z80->pc++];
		break;
	case 0x12:	/* stax d */
		z80->ram[((z80->d) << 8) | z80->e] = z80->a;
		break;
	case 0x13:	/* inx d */
		z80->e++;
		if (z80->e == 0)	/* overflow */
			z80->d++;
		break;
	case 0x14:	/* inr d */
		z80->d++;
		if ((z80->d & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->d);
		break;
	case 0x15:	/* dcr d */
		z80->d--;
		if ((z80->d & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->d);
		break;
	case 0x16:	/* mvi d, i8 */
		z80->d = z80->ram[z80->pc++];
		break;
	case 0x17:	/* ral */
		carry = (z80->a) << 1;
		z80->a = carry & 0xff;
		if (z80->fcy == 1)
			z80->a++;

		if (carry > 0xff)
			z80->fcy = 1;
		else
			z80->fcy = 0;
		break;
	case 0x19:	/* dad d */
		sb1 = ((z80->d) << 8) | z80->e;
		sb2 = ((z80->h) << 8) | z80->l;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			z80->fcy = 1;
		else
			z80->fcy = 0;

		z80->h = (doublecarry >> 8) & 0xff;
		z80->l = doublecarry & 0xff;
		break;
	case 0x1a:	/* ldax d */
		z80->a = z80->ram[((z80->d) << 8) | z80->e];
		break;
	case 0x1b:	/* dcx d */
		z80->e--;
		if (z80->e == 0xff)	/* underflow */
			z80->d--;
		break;
	case 0x1c:	/* inr e */
		z80->e++;
		if ((z80->e & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->e);
		break;
	case 0x1d:	/* dcr e */
		z80->e--;
		if ((z80->e & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->e);
		break;
	case 0x1e:	/* mvi e, i8 */
		z80->e = z80->ram[z80->pc++];
		break;
	case 0x1f:	/* rar */
		carry = (z80->a) & 0x1;
		z80->a = (z80->a) >> 1;
		if (z80->fcy == 1)
			z80->a += 0x80;

		if (carry)
			z80->fcy = 1;
		else
			z80->fcy = 0;
		break;
	case 0x21:	/* lxi h, i16 */
		z80->l = z80->ram[z80->pc++];
		z80->h = z80->ram[z80->pc++];
		break;
	case 0x22:	/* shld i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		z80->ram[sb1++] = z80->l;
		z80->ram[sb1] = z80->h;
		break;
	case 0x23:	/* inx h */
		z80->l++;
		if (z80->l == 0)	/* overflow */
			z80->h++;
		break;
	case 0x24:	/* inr h */
		z80->h++;
		if ((z80->h & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->h);
		break;
	case 0x25:	/* dcr h */
		z80->h--;
		if ((z80->h & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->h);
		break;
	case 0x26:	/* mvi h, i8 */
		z80->h = z80->ram[z80->pc++];
		break;
	case 0x27:	/* daa */
		daa(z80);
		break;
	case 0x29:	/* dad h */
		sb1 = ((z80->h) << 8) | z80->l;
		sb2 = sb1;
		doublecarry = sb1 + sb2;

		if (doublecarry > 0xffff)
			z80->fcy = 1;
		else
			z80->fcy = 0;

		z80->h = (doublecarry >> 8) & 0xff;
		z80->l = doublecarry & 0xff;
		break;
	case 0x2a:	/* lhld i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		z80->l = z80->ram[sb1++];
		z80->h = z80->ram[sb1];
		break;
	case 0x2b:	/* dcx h */
		z80->l--;
		if (z80->l == 0xff)	/* underflow */
			z80->h--;
		break;
	case 0x2c:	/* inr l */
		z80->l++;
		if ((z80->l & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->l);
		break;
	case 0x2d:	/* dcr l */
		z80->l--;
		if ((z80->l & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->l);
		break;
	case 0x2e:	/* mvi l, i8 */
		z80->l = z80->ram[z80->pc++];
		break;
	case 0x2f:	/* cma */
		z80->a = ~(z80->a);
		break;
	case 0x31:	/* lxi sp, i16 */
		z80->sp = z80->ram[z80->pc++];
		z80->sp |= z80->ram[z80->pc++] << 8;
		break;
	case 0x32:	/* sta i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		z80->ram[sb1] = z80->a;
		break;
	case 0x33:	/* inx sp */
		++z80->sp;
		break;
	case 0x34:	/* inr m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1]++;
		if ((z80->ram[sb1] & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->ram[sb1]);
		break;
	case 0x35:	/* dcr m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1]--;
		if ((z80->ram[sb1] & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->ram[sb1]);
		break;
	case 0x36:	/* mvi m, i8 */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->ram[z80->pc++];
		break;
	case 0x37:	/* stc */
		z80->fcy = 1;
		break;
	case 0x39:	/* dad sp */
		sb1 = ((z80->h) << 8) | z80->l;
		doublecarry = z80->sp + sb1;

		if (doublecarry > 0xffff)
			z80->fcy = 1;
		else
			z80->fcy = 0;

		z80->h = (doublecarry >> 8) & 0xff;
		z80->l = doublecarry & 0xff;
		break;
	case 0x3a:	/* lda i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		z80->a = z80->ram[sb1];
		break;
	case 0x3b:	/* dcx sp */
		--z80->sp;
		break;
	case 0x3c:	/* inr a */
		z80->a++;
		if ((z80->a & 0xf) == 0)
			z80->fac = 1;
		else
			z80->fac = 0;

		flags(z80, z80->a);
		break;
	case 0x3d:	/* dcr a */
		z80->a--;
		if ((z80->a & 0xf) == 0xf)
			z80->fac = 0;
		else
			z80->fac = 1;

		flags(z80, z80->a);
		break;
	case 0x3e:	/* mvi a, i8 */
		z80->a = z80->ram[z80->pc++];
		break;
	case 0x3f:	/* cmc */
		if (z80->fcy == 1)
			z80->fcy = 0;
		else
			z80->fcy = 1;
		break;
	case 0x40:	/* mov b, b */
		/* cheat, do nothing */
		break;
	case 0x41:	/* mov b, c */
		z80->b = z80->c;
		break;
	case 0x42:	/* mov b, d */
		z80->b = z80->d;
		break;
	case 0x43:	/* mov b, e */
		z80->b = z80->e;
		break;
	case 0x44:	/* mov b, h */
		z80->b = z80->h;
		break;
	case 0x45:	/* mov b, l */
		z80->b = z80->l;
		break;
	case 0x46:	/* mov b, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->b = z80->ram[sb1];
		break;
	case 0x47:	/* mov b, a */
		z80->b = z80->a;
		break;
	case 0x48:	/* mov c, b */
		z80->c = z80->b;
		break;
	case 0x49:	/* mov c, c */
		/* cheat, do nothing */
		break;
	case 0x4a:	/* mov c, d */
		z80->c = z80->d;
		break;
	case 0x4b:	/* mov c, e */
		z80->c = z80->e;
		break;
	case 0x4c:	/* mov c, h */
		z80->c = z80->h;
		break;
	case 0x4d:	/* mov c, l */
		z80->c = z80->l;
		break;
	case 0x4e:	/* mov c, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->c = z80->ram[sb1];
		break;
	case 0x4f:	/* mov c, a */
		z80->c = z80->a;
		break;
	case 0x50:	/* mov d, b */
		z80->d = z80->b;
		break;
	case 0x51:	/* mov d, c */
		z80->d = z80->c;
		break;
	case 0x52:	/* mov d, d */
		/* cheat, do nothing */
		break;
	case 0x53:	/* mov d, e */
		z80->d = z80->e;
		break;
	case 0x54:	/* mov d, h */
		z80->d = z80->h;
		break;
	case 0x55:	/* mov d, l */
		z80->d = z80->l;
		break;
	case 0x56:	/* mov d, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->d = z80->ram[sb1];
		break;
	case 0x57:	/* mov d, a */
		z80->d = z80->a;
		break;
	case 0x58:	/* mov e, b */
		z80->e = z80->b;
		break;
	case 0x59:	/* mov e, c */
		z80->e = z80->c;
		break;
	case 0x5a:	/* mov e, d */
		z80->e = z80->d;
		break;
	case 0x5b:	/* mov e, e */
		/* cheat, do nothing */
		break;
	case 0x5c:	/* mov e, h */
		z80->e = z80->h;
		break;
	case 0x5d:	/* mov e, l */
		z80->e = z80->l;
		break;
	case 0x5e:	/* mov e, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->e = z80->ram[sb1];
		break;
	case 0x5f:	/* mov e, a */
		z80->e = z80->a;
		break;
	case 0x60:	/* mov h, b */
		z80->h = z80->b;
		break;
	case 0x61:	/* mov h, c */
		z80->h = z80->c;
		break;
	case 0x62:	/* mov h, d */
		z80->h = z80->d;
		break;
	case 0x63:	/* mov h, e */
		z80->h = z80->e;
		break;
	case 0x64:	/* mov h, h */
		/* cheat, do nothing */
		break;
	case 0x65:	/* mov h, l */
		z80->h = z80->l;
		break;
	case 0x66:	/* mov h, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->h = z80->ram[sb1];
		break;
	case 0x67:	/* mov h, a */
		z80->h = z80->a;
		break;
	case 0x68:	/* mov l, b */
		z80->l = z80->b;
		break;
	case 0x69:	/* mov l, c */
		z80->l = z80->c;
		break;
	case 0x6a:	/* mov l, d */
		z80->l = z80->d;
		break;
	case 0x6b:	/* mov l, e */
		z80->l = z80->e;
		break;
	case 0x6c:	/* mov l, h */
		z80->l = z80->h;
		break;
	case 0x6d:	/* mov l, l */
		/* cheat, do nothing */
		break;
	case 0x6e:	/* mov l, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->l = z80->ram[sb1];
		break;
	case 0x6f:	/* mov l, a */
		z80->l = z80->a;
		break;
	case 0x70:	/* mov m, b */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->b;
		break;
	case 0x71:	/* mov m, c */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->c;
		break;
	case 0x72:	/* mov m, d */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->d;
		break;
	case 0x73:	/* mov m, e */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->e;
		break;
	case 0x74:	/* mov m, h */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->h;
		break;
	case 0x75:	/* mov m, l */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->l;
		break;
	case 0x76:	/* hlt */
		return 0;
	case 0x77:	/* mov m, a */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->ram[sb1] = z80->a;
		break;
	case 0x78:	/* mov a, b */
		z80->a = z80->b;
		break;
	case 0x79:	/* mov a, c */
		z80->a = z80->c;
		break;
	case 0x7a:	/* mov a, d */
		z80->a = z80->d;
		break;
	case 0x7b:	/* mov a, e */
		z80->a = z80->e;
		break;
	case 0x7c:	/* mov a, h */
		z80->a = z80->h;
		break;
	case 0x7d:	/* mov a, l */
		z80->a = z80->l;
		break;
	case 0x7e:	/* mov a, m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->a = z80->ram[sb1];
		break;
	case 0x7f:	/* mov a, a */
		/* cheat, do nothing */
		break;
	case 0x80:	/* add b */
		carry = z80->a + z80->b;
		carryflag(z80, z80->a, z80->b, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x81:	/* add c */
		carry = z80->a + z80->c;
		carryflag(z80, z80->a, z80->c, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x82:	/* add d */
		carry = z80->a + z80->d;
		carryflag(z80, z80->a, z80->d, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x83:	/* add e */
		carry = z80->a + z80->e;
		carryflag(z80, z80->a, z80->e, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x84:	/* add h */
		carry = z80->a + z80->h;
		carryflag(z80, z80->a, z80->h, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x85:	/* add l */
		carry = z80->a + z80->l;
		carryflag(z80, z80->a, z80->l, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x86:	/* add m */
		sb1 = ((z80->h) << 8) | z80->l;
		carry = z80->a + z80->ram[sb1];
		carryflag(z80, z80->a, z80->ram[sb1], carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x87:	/* add a */
		carry = z80->a + z80->a;
		carryflag(z80, z80->a, z80->a, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x88:	/* adc b */
		carry = z80->a + z80->b + z80->fcy;
		carryflag(z80, z80->a, z80->b, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x89:	/* adc c */
		carry = z80->a + z80->c + z80->fcy;
		carryflag(z80, z80->a, z80->c, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8a:	/* adc d */
		carry = z80->a + z80->d + z80->fcy;
		carryflag(z80, z80->a, z80->d, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8b:	/* adc e */
		carry = z80->a + z80->e + z80->fcy;
		carryflag(z80, z80->a, z80->e, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8c:	/* adc h */
		carry = z80->a + z80->h + z80->fcy;
		carryflag(z80, z80->a, z80->h, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8d:	/* adc l */
		carry = z80->a + z80->l + z80->fcy;
		carryflag(z80, z80->a, z80->l, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8e:	/* adc m */
		sb1 = ((z80->h) << 8) | z80->l;
		carry = z80->a + z80->ram[sb1] + z80->fcy;
		carryflag(z80, z80->a, z80->ram[sb1], carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x8f:	/* adc a */
		carry = z80->a + z80->a + z80->fcy;
		carryflag(z80, z80->a, z80->a, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x90:	/* sub b */
		carry = z80->a + ~(z80->b) + 1;
		carryflag(z80, z80->a, ~(z80->b), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x91:	/* sub c */
		carry = z80->a + ~(z80->c) + 1;
		carryflag(z80, z80->a, ~(z80->c), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x92:	/* sub d */
		carry = z80->a + ~(z80->d) + 1;
		carryflag(z80, z80->a, ~(z80->d), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x93:	/* sub e */
		carry = z80->a + ~(z80->e) + 1;
		carryflag(z80, z80->a, ~(z80->e), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x94:	/* sub h */
		carry = z80->a + ~(z80->h) + 1;
		carryflag(z80, z80->a, ~(z80->h), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x95:	/* sub l */
		carry = z80->a + ~(z80->l) + 1;
		carryflag(z80, z80->a, ~(z80->l), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x96:	/* sub m */
		sb1 = ((z80->h) << 8) | z80->l;
		carry = z80->a + ~(z80->ram[sb1]) + 1;
		carryflag(z80, z80->a, ~(z80->ram[sb1]), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x97:	/* sub a */
		carry = z80->a + ~(z80->a) + 1;
		carryflag(z80, z80->a, ~(z80->a), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x98:	/* sbb b */
		carry = z80->a + ~(z80->b) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->b), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x99:	/* sbb c */
		carry = z80->a + ~(z80->c) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->c), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9a:	/* sbb d */
		carry = z80->a + ~(z80->d) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->d), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9b:	/* sbb e */
		carry = z80->a + ~(z80->e) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->e), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9c:	/* sbb h */
		carry = z80->a + ~(z80->h) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->h), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9d:	/* sbb l */
		carry = z80->a + ~(z80->l) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->l), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9e:	/* sbb m */
		sb1 = ((z80->h) << 8) | z80->l;
		carry = z80->a + ~(z80->ram[sb1]) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->ram[sb1]), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0x9f:	/* sbb a */
		carry = z80->a + ~(z80->a) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(z80->a), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0xa0:	/* ana b */
		z80->a = z80->a & z80->b;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa1:	/* ana c */
		z80->a = z80->a & z80->c;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa2:	/* ana d */
		z80->a = z80->a & z80->d;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa3:	/* ana e */
		z80->a = z80->a & z80->e;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa4:	/* ana h */
		z80->a = z80->a & z80->h;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa5:	/* ana l */
		z80->a = z80->a & z80->l;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa6:	/* ana m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->a = z80->a & z80->ram[sb1];
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa7:	/* ana a */
		z80->a = z80->a & z80->a;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa8:	/* xra b */
		z80->a = z80->a ^ z80->b;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xa9:	/* xra c */
		z80->a = z80->a ^ z80->c;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xaa:	/* xra d */
		z80->a = z80->a ^ z80->d;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xab:	/* xra e */
		z80->a = z80->a ^ z80->e;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xac:	/* xra h */
		z80->a = z80->a ^ z80->h;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xad:	/* xra l */
		z80->a = z80->a ^ z80->l;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xae:	/* xra m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->a = z80->a ^ z80->ram[sb1];
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xaf:	/* xra a */
		z80->a = z80->a ^ z80->a;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb0:	/* ora b */
		z80->a = z80->a | z80->b;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb1:	/* ora c */
		z80->a = z80->a | z80->c;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb2:	/* ora d */
		z80->a = z80->a | z80->d;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb3:	/* ora e */
		z80->a = z80->a | z80->e;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb4:	/* ora h */
		z80->a = z80->a | z80->h;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb5:	/* ora l */
		z80->a = z80->a | z80->l;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb6:	/* ora m */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->a = z80->a | z80->ram[sb1];
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb7:	/* ora a */
		z80->a = z80->a | z80->a;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xb8:	/* cmp b */
		carry = z80->a + ~(z80->b) + 1;
		carryflag(z80, z80->a, ~(z80->b), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xb9:	/* cmp c */
		carry = z80->a + ~(z80->c) + 1;
		carryflag(z80, z80->a, ~(z80->c), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xba:	/* cmp d */
		carry = z80->a + ~(z80->d) + 1;
		carryflag(z80, z80->a, ~(z80->d), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xbb:	/* cmp e */
		carry = z80->a + ~(z80->e) + 1;
		carryflag(z80, z80->a, ~(z80->e), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xbc:	/* cmp h */
		carry = z80->a + ~(z80->h) + 1;
		carryflag(z80, z80->a, ~(z80->h), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xbd:	/* cmp l */
		carry = z80->a + ~(z80->l) + 1;
		carryflag(z80, z80->a, ~(z80->l), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xbe:	/* cmp m */
		sb1 = ((z80->h) << 8) | z80->l;
		carry = z80->a + ~(z80->ram[sb1]) + 1;
		carryflag(z80, z80->a, ~(z80->ram[sb1]), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xbf:	/* cmp a */
		carry = z80->a + ~(z80->a) + 1;
		carryflag(z80, z80->a, ~(z80->a), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xc0:	/* rnz */
		if (z80->fz == 0)
			ret(z80);
		break;
	case 0xc1:	/* pop b */
		z80->c = z80->ram[z80->sp++];
		z80->b = z80->ram[z80->sp++];
		break;
	case 0xc2:	/* jnz i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fz == 0)
			z80->pc = sb1;
		break;
	case 0xc3:	/* jmp i16 */
	case 0xcb:
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		z80->pc = sb1;
		break;
	case 0xc4:	/* cnz i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fz == 0) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xc5:	/* push b */
		z80->ram[--z80->sp] = z80->b;
		z80->ram[--z80->sp] = z80->c;
		break;
	case 0xc6:	/* adi i8 */
		halfcarry = z80->ram[z80->pc++];
		carry = z80->a + halfcarry;
		carryflag(z80, z80->a, halfcarry, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0xc7:	/* rst 0 */
		call(z80);
		z80->pc = 0x00;
		break;
	case 0xc8:	/* rz */
		if (z80->fz == 1)
			ret(z80);
		break;
	case 0xc9:	/* ret */
		ret(z80);
		break;
	case 0xca:	/* jz i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fz == 1)
			z80->pc = sb1;
		break;
	case 0xcc:	/* cz i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fz == 1) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xcd:	/* call i16 */
	case 0xdd:
	case 0xed:
	case 0xfd:
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		call(z80);
		z80->pc = sb1;
		break;
	case 0xce:	/* aci i8 */
		halfcarry = z80->ram[z80->pc++];
		carry = z80->a + halfcarry + z80->fcy;
		carryflag(z80, z80->a, halfcarry, carry, AC_ADD);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0xcf:	/* rst 1 */
		call(z80);
		z80->pc = 0x08;
		break;
	case 0xd0:	/* rnc */
		if (z80->fcy == 0)
			ret(z80);
		break;
	case 0xd1:	/* pop d */
		z80->e = z80->ram[z80->sp++];
		z80->d = z80->ram[z80->sp++];
		break;
	case 0xd2:	/* jnc i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fcy == 0)
			z80->pc = sb1;
		break;
	case 0xd3:	/* out i8 */
		z80->port = z80->ram[z80->pc++];
		z80->inout[z80->port] = z80->a;
		break;
	case 0xd4:	/* cnc i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fcy == 0) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xd5:	/* push d */
		z80->ram[--z80->sp] = z80->d;
		z80->ram[--z80->sp] = z80->e;
		break;
	case 0xd6:	/* sui i8 */
		halfcarry = z80->ram[z80->pc++];
		carry = z80->a + ~(halfcarry) + 1;
		carryflag(z80, z80->a, ~(halfcarry), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0xd7:	/* rst 2 */
		call(z80);
		z80->pc = 0x10;
		break;
	case 0xd8:	/* rc */
		if (z80->fcy == 1)
			ret(z80);
		break;
	case 0xd9:
		exx(z80);
		break;
	case 0xda:	/* jc i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fcy == 1)
			z80->pc = sb1;
		break;
	case 0xdb:	/* in i8 */
		z80->port = z80->ram[z80->pc++];
		break;
	case 0xdc:	/* cc i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fcy == 1) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xde:	/* sbi i8 */
		halfcarry = z80->ram[z80->pc++];
		carry = z80->a + ~(halfcarry) + 1 - z80->fcy;
		carryflag(z80, z80->a, ~(halfcarry), carry, AC_SUB);
		z80->a = carry & 0xff;
		flags(z80, z80->a);
		break;
	case 0xdf:	/* rst 3 */
		call(z80);
		z80->pc = 0x18;
		break;
	case 0xe0:	/* rpo */
		if (z80->fp == 0)
			ret(z80);
		break;
	case 0xe1:	/* pop h */
		z80->l = z80->ram[z80->sp++];
		z80->h = z80->ram[z80->sp++];
		break;
	case 0xe2:	/* jpo i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fp == 0)
			z80->pc = sb1;
		break;
	case 0xe3:	/* xthl */
		sb1 = ((z80->h) << 8) | z80->l;
		z80->l = z80->ram[z80->sp];
		z80->h = z80->ram[z80->sp + 1];
		z80->ram[z80->sp] = sb1 & 0xff;
		z80->ram[z80->sp + 1] = (sb1 >> 8) & 0xff;
		break;
	case 0xe4:	/* cpo i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fp == 0) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xe5:	/* push h */
		z80->ram[--z80->sp] = z80->h;
		z80->ram[--z80->sp] = z80->l;
		break;
	case 0xe6:	/* ani i8 */
		z80->a = z80->a & z80->ram[z80->pc++];
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xe7:	/* rst 4 */
		call(z80);
		z80->pc = 0x20;
		break;
	case 0xe8:	/* rpe */
		if (z80->fp == 1)
			ret(z80);
		break;
	case 0xe9:	/* pchl */
		z80->pc = ((z80->h) << 8) | z80->l;
		break;
	case 0xea:	/* jpe i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fp == 1)
			z80->pc = sb1;
		break;
	case 0xeb:	/* xchg */
		sb1 = ((z80->d) << 8) | z80->e;
		z80->d = z80->h;
		z80->e = z80->l;
		z80->h = (sb1 >> 8) & 0xff;
		z80->l = sb1 & 0xff;
		break;
	case 0xec:	/* cpe i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fp == 1) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xee:	/* xri i8 */
		halfcarry = z80->ram[z80->pc++];
		z80->a = z80->a ^ halfcarry;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xef:	/* rst 5 */
		call(z80);
		z80->pc = 0x28;
		break;
	case 0xf0:	/* rp */
		if (z80->fs == 0)
			ret(z80);
		break;
	case 0xf1:	/* pop psw */
		z80->fs = (z80->ram[z80->sp] >> 7) & 0x1;
		z80->fz = (z80->ram[z80->sp] >> 6) & 0x1;
		z80->fzero = 0;
		z80->fac = (z80->ram[z80->sp] >> 4) & 0x1;
		z80->fzerox = 0;
		z80->fp = (z80->ram[z80->sp] >> 2) & 0x1;
		z80->fone = 1;
		z80->fcy = z80->ram[z80->sp++] & 0x1;
		z80->a = z80->ram[z80->sp++];
		break;
	case 0xf2:	/* jp i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fs == 0)
			z80->pc = sb1;
		break;
	case 0xf3:	/* di */
		z80->inte = 0;
		break;
	case 0xf4:	/* cp i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fs == 0) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xf5:	/* push psw */
		z80->ram[--z80->sp] = z80->a;
		z80->ram[--z80->sp] = z80->fs << 7;
		z80->ram[z80->sp] |= z80->fz << 6;
		z80->ram[z80->sp] |= z80->fzero << 5;
		z80->ram[z80->sp] |= z80->fac << 4;
		z80->ram[z80->sp] |= z80->fzerox << 3;
		z80->ram[z80->sp] |= z80->fp << 2;
		z80->ram[z80->sp] |= z80->fone << 1;
		z80->ram[z80->sp] |= z80->fcy;
		break;
	case 0xf6:	/* ori i8 */
		halfcarry = z80->ram[z80->pc++];
		z80->a = z80->a | halfcarry;
		flags(z80, z80->a);
		z80->fac = 0;
		z80->fcy = 0;
		break;
	case 0xf7:	/* rst 6 */
		call(z80);
		z80->pc = 0x30;
		break;
	case 0xf8:	/* rm */
		if (z80->fs == 1)
			ret(z80);
		break;
	case 0xf9:	/* sphl */
		z80->sp = ((z80->h) << 8) | z80->l;
		break;
	case 0xfa:	/* jm i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fs == 1)
			z80->pc = sb1;
		break;
	case 0xfb:	/* ei */
		z80->inte = 1;
		break;
	case 0xfc:	/* cm i16 */
		sb1 = z80->ram[z80->pc++];
		sb1 |= z80->ram[z80->pc++] << 8;
		if (z80->fs == 1) {
			call(z80);
			z80->pc = sb1;
		}
		break;
	case 0xfe:	/* cpi i8 */
		halfcarry = z80->ram[z80->pc++];
		carry = z80->a + ~(halfcarry) + 1;
		carryflag(z80, z80->a, ~(halfcarry), carry, AC_SUB);
		flags(z80, carry);
		break;
	case 0xff:	/* rst 7 */
		call(z80);
		z80->pc = 0x38;
		break;
	}

	return 1;
}

static void
reset(struct cpu *z80)
{
	unsigned int i;

	z80->a = 0;
	z80->ap = 0;
	z80->b = 0;
	z80->bp = 0;
	z80->c = 0;
	z80->cp = 0;
	z80->d = 0;
	z80->dp = 0;
	z80->e = 0;
	z80->ep = 0;
	z80->h = 0;
	z80->hp = 0;
	z80->l = 0;
	z80->lp = 0;

	z80->fs = 0;
	z80->fsp = 0;
	z80->fz = 1;
	z80->fzp = 1;
	z80->fzero = 0;
	z80->fzerop = 0;
	z80->fac = 0;
	z80->facp = 0;
	z80->fzerox = 0;
	z80->fzeroxp = 0;
	z80->fp = 1;
	z80->fpp = 1;
	z80->fone = 1;
	z80->fonep = 1;
	z80->fcy = 0;
	z80->fcyp = 0;

	z80->pc = 0;
	z80->sp = 0;

	z80->inte = 0;

	for (i = 0; i < sizeof(z80->ram); i++)
		z80->ram[i] = '\0';

	for (i = 0; i < sizeof(z80->inout); i++)
		z80->inout[i] = '\0';

	z80->port = -1;
}

/*
 * World's smallest CP/M
 */
static void
cpm(struct cpu *z80)
{

	z80->ram[0] = 0x76;

	z80->ram[5] = 0xd3;
	z80->ram[6] = 0x00;
	z80->ram[7] = 0xc9;
}

int
main(int argc, char *argv[])
{
	struct cpu z80;
	unsigned int i = 0x100;
	int ch, fd;
	word addr, save, size;

	if (argc != 2)
		return 1;

	reset(&z80);
	cpm(&z80);

	fd = open(argv[1], O_RDONLY);
	while (i < sizeof(z80.ram))
		read(fd, &z80.ram[i++], 1);
	close(fd);

	z80.pc = 0x100;
	while (execute(&z80, z80.ram[z80.pc++])) {
		if (z80.port == 0) {
			switch (z80.c) {
			case 0:		/* P_TERMCPM */
				reset(&z80);
				return 0;
			case 1:		/* C_READ */
				while (read(0, &z80.l, 1) < 1)
					;
				z80.a = z80.l;
				write(1, &z80.a, 1);
				break;
			case 2:		/* C_WRITE */
				write(1, &z80.e, 1);
				break;
			case 3:		/* A_READ */
				z80.l = 0;
				z80.a = z80.l;
				break;
			case 4:		/* A_WRITE */
				write(2, &z80.e, 1);
				break;
			case 5:		/* L_WRITE */
				write(2, &z80.e, 1);
				break;
			case 6:		/* C_RAWIO */
				fd = fcntl(0, F_GETFL);
				fcntl(0, F_SETFL, fd | O_NONBLOCK);
				if (read(0, &z80.l, 1) < 1)
					z80.l = 0;
				fcntl(0, F_SETFL, fd & ~(O_NONBLOCK));
				z80.a = z80.l;
				break;
			case 7:		/* Get I/O byte */
				break;
			case 8:		/* Set I/O byte */
				break;
			case 9:		/* C_WRITESTR */
				addr = (z80.d << 8) | z80.e;
				while (z80.ram[addr] != '$')
					write(1, &z80.ram[addr++], 1);
				break;
			case 10:	/* C_READSTR */
				addr = (z80.d << 8) | z80.e;
				size = z80.ram[addr];
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
						z80.ram[addr] = ch;

					write(1, &z80.ram[addr++], 1);
				}

				addr = addr - save + 1;
				z80.ram[save + 1] = addr & 0xff;

				break;
			case 12:	/* S_BDOSVER */
				z80.h = 0;
				z80.b = z80.h;

				z80.l = 0x22;
				z80.a = z80.l;
				break;
			case 25:	/* DRV_GET */
				z80.a = 0;
			}

			z80.port = -1;
		}
	}
}
