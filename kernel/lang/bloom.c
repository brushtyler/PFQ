/***************************************************************
 *
 * (C) 2011-16 Nicola Bonelli <nicola@pfq.io>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <lang/module.h>
#include <lang/qbuff.h>
#include <lang/bloom.h>

#include <pfq/printk.h>

static bool
bloom_src(arguments_t args, struct qbuff * buff)
{
	struct iphdr _iph;
	const struct iphdr *ip;
	uint32_t fold, addr;
	__be32 mask;
	char *mem;

	ip = qbuff_ip_header_pointer(buff, 0, sizeof(_iph), &_iph);
	if (ip == NULL)
		return false;

	fold = GET_ARG_0(uint32_t, args);
	mem  = GET_ARG_1(char *,   args);
	mask = GET_ARG_2(__be32,   args);

	addr = be32_to_cpu(ip->saddr & mask);

	return ( BF_TEST(mem, hfun1(addr) & fold) &&
	         BF_TEST(mem, hfun2(addr) & fold) &&
	         BF_TEST(mem, hfun3(addr) & fold) &&
	         BF_TEST(mem, hfun4(addr) & fold) );
}


static bool
bloom_dst(arguments_t args, struct qbuff * buff)
{
	struct iphdr _iph;
	const struct iphdr *ip;
	uint32_t fold, addr;
	__be32 mask;
	char *mem;

	ip = qbuff_ip_header_pointer(buff, 0, sizeof(_iph), &_iph);
	if (ip == NULL)
		return false;

	fold = GET_ARG_0(uint32_t, args);
	mem  = GET_ARG_1(char *,   args);
	mask = GET_ARG_2(__be32,   args);

	addr = be32_to_cpu(ip->daddr & mask);

	return ( BF_TEST(mem, hfun1(addr) & fold) &&
	         BF_TEST(mem, hfun2(addr) & fold) &&
	         BF_TEST(mem, hfun3(addr) & fold) &&
	         BF_TEST(mem, hfun4(addr) & fold) );
}

static bool
bloom(arguments_t args, struct qbuff * buff)
{
	struct iphdr _iph;
	const struct iphdr *ip;
	uint32_t fold, addr;
	__be32 mask;
	char *mem;

	ip = qbuff_ip_header_pointer(buff, 0, sizeof(_iph), &_iph);
	if (ip == NULL)
		return false;

	fold = GET_ARG_0(uint32_t, args);
	mem  = GET_ARG_1(char *,   args);
	mask = GET_ARG_2(__be32,   args);

	if (buff->monad->ep_ctx & EPOINT_DST)
	{
		addr = be32_to_cpu(ip->daddr & mask);

		if ( BF_TEST(mem, hfun1(addr) & fold) &&
		     BF_TEST(mem, hfun2(addr) & fold) &&
		     BF_TEST(mem, hfun3(addr) & fold) &&
		     BF_TEST(mem, hfun4(addr) & fold) )
			return true;
	}

	if (buff->monad->ep_ctx & EPOINT_SRC)
	{
		addr = be32_to_cpu(ip->saddr & mask);

		if ( BF_TEST(mem, hfun1(addr) & fold) &&
		     BF_TEST(mem, hfun2(addr) & fold) &&
		     BF_TEST(mem, hfun3(addr) & fold) &&
		     BF_TEST(mem, hfun4(addr) & fold) )
			return true;
	}

	return false;
}


static ActionQbuff
bloom_filter(arguments_t args, struct qbuff * buff)
{
	if (bloom(args, buff))
		return Pass(buff);
	return Drop(buff);
}


static ActionQbuff
bloom_src_filter(arguments_t args, struct qbuff * buff)
{
	if (bloom_src(args, buff))
		return Pass(buff);
	return Drop(buff);
}

static ActionQbuff
bloom_dst_filter(arguments_t args, struct qbuff * buff)
{
	if (bloom_dst(args, buff))
		return Pass(buff);
	return Drop(buff);
}


static int bloom_init(arguments_t args)
{
	unsigned int m = GET_ARG_0(unsigned int, args);
	size_t n = LEN_ARRAY_1(args);
	__be32 *ips = GET_ARRAY_1(__be32, args);
	__be32 mask;
	size_t i;

	char *mem;

	m = clp2(m);

	/* set bloom filter fold mask */

	SET_ARG_0(args, m-1);

	if (m > (1UL << 24)) {
		printk(KERN_INFO "[PFQ|init] bloom filter: maximum number of bins exceeded (2^24)!\n");
		return -EPERM;
	}

	mem = kzalloc(m >> 3, GFP_KERNEL);
	if (!mem) {
		printk(KERN_INFO "[PFQ|init] bloom filter: out of memory!\n");
		return -ENOMEM;
	}

	/* set bloom filter memory */

	SET_ARG_1(args, mem);

	mask = inet_make_mask(GET_ARG_2(int, args));

	/* set network mask */

	SET_ARG_2(args, mask);

	pr_devel("[PFQ|init] bloom filter@%p: k=4, n=%zu, m=%u size=%u netmask=%pI4 bytes.\n", mem, n, m, m>>3, &mask);

	for(i = 0; i < n; i++)
	{
		uint32_t h1 = hfun1(be32_to_cpu(ips[i] & mask)) & (m-1);
		uint32_t h2 = hfun2(be32_to_cpu(ips[i] & mask)) & (m-1);
		uint32_t h3 = hfun3(be32_to_cpu(ips[i] & mask)) & (m-1);
		uint32_t h4 = hfun4(be32_to_cpu(ips[i] & mask)) & (m-1);

		BF_SET(mem, h1);
		BF_SET(mem, h2);
		BF_SET(mem, h3);
		BF_SET(mem, h4);

		pr_devel("[PFQ|init] bloom filter: -> set address %pI4\n", ips+i);
	}

	return 0;
}


static int bloom_fini(arguments_t args)
{
	char *mem = GET_ARG_1(char *, args);

	kfree(mem);
	pr_devel("[PFQ|init] bloom filter: memory freed@%p!\n", mem);

	return 0;
}


struct pfq_lang_function_descr bloom_functions[] = {

	{"bloom",		"CInt -> [Word32] -> CInt -> Qbuff -> Bool",		bloom,			bloom_init,	bloom_fini},
	{"bloom_src",		"CInt -> [Word32] -> CInt -> Qbuff -> Bool",		bloom_src,		bloom_init,	bloom_fini},
	{"bloom_dst",		"CInt -> [Word32] -> CInt -> Qbuff -> Bool",		bloom_dst,		bloom_init,	bloom_fini},
	{"bloom_filter",	"CInt -> [Word32] -> CInt -> Qbuff -> Action Qbuff",	bloom_filter,		bloom_init,	bloom_fini},
	{"bloom_src_filter",	"CInt -> [Word32] -> CInt -> Qbuff -> Action Qbuff",	bloom_src_filter,	bloom_init,	bloom_fini},
	{"bloom_dst_filter",	"CInt -> [Word32] -> CInt -> Qbuff -> Action Qbuff",	bloom_dst_filter,	bloom_init,	bloom_fini},
	{ NULL }};

