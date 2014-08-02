/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
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

#ifndef _PF_Q_BITOPS_H_
#define _PF_Q_BITOPS_H_

#include <asm/bitops.h>

#define pfq_ctz(n) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned int),        (unsigned int)__builtin_ctz(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long),       (unsigned int)__builtin_ctzl(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long long),  (unsigned int)__builtin_ctzll(n), (void)0 )))


#ifdef __LP64__
#define pfq_popcount(n) \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned int),        (unsigned int)hweight32(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long),       (unsigned int)hweight64(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long long),  (unsigned int)hweight64(n), (void)0)))
#else
#define pfq_popcount(n) \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned int),        (unsigned int)hweight32(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long),       (unsigned int)hweight32(n), \
        __builtin_choose_expr(__builtin_types_compatible_p(typeof(n),unsigned long long),  (unsigned int)hweight64(n), (void)0)))
#endif


#define pfq_bitwise_foreach(m, n, ...) \
{ \
        typeof(m) _mask = (m); \
	for(; n = _mask & -_mask, _mask ; _mask^=n) \
	__VA_ARGS__ \
}


#endif /* _PF_Q_BITOPS_H_ */
