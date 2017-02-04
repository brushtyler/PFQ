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

#include <pragma/diagnostic_push>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <pragma/diagnostic_pop>

#include <core/define.h>
#include <core/sock.h>

#include <pfq/thread.h>
#include <pfq/memory.h>
#include <pfq/io.h>


static DEFINE_MUTEX(pfq_thread_tx_pool_lock);
static DEFINE_MUTEX(pfq_thread_rx_pool_lock);


static struct pfq_thread_tx_data pfq_thread_tx_pool[Q_CORE_MAX_CPU] =
{
	[0 ... Q_CORE_MAX_CPU-1] = {
		.id	= -1,
		.cpu    = -1,
		.task	= NULL,
		.sock   = {NULL, NULL, NULL, NULL},
		.sock_queue = {{-1}, {-1}, {-1}, {-1}}
	}
};


static struct pfq_thread_rx_data pfq_thread_rx_pool[Q_CORE_MAX_CPU] =
{
	[0 ... Q_CORE_MAX_CPU-1] = {
		.id   = -1,
		.cpu  = -1,
		.task = NULL,
		.napi = {-1, -1, -1, -1},
		.napi_nr = 0
	}
};


#ifdef PFQ_DEBUG
static void
pfq_thread_ping(const char *type, struct pfq_thread_data const *data)
{
	printk( KERN_INFO "%s[%d] cpu=%d task=%p (PING)!\n"
	       , type
	       , data->id
	       , data->cpu
	       , data->task);
}
#endif



static int
pfq_tx_thread(void *_data)
{
	struct pfq_thread_tx_data *data = (struct pfq_thread_tx_data *)_data;

#ifdef PFQ_DEBUG
        int now = 0;
#endif

	if (data == NULL) {
		printk(KERN_INFO "[PFQ] Tx thread data error!\n");
		return -EPERM;
	}

	printk(KERN_INFO "[PFQ] Tx[%d] thread started on cpu %d.\n", data->id, data->cpu);

	__set_current_state(TASK_RUNNING);

        for(;;)
	{
		/* transmit the registered socket's queues */
		bool reg = false;
		int total_sent = 0, n;

		for(n = 0; n < Q_MAX_TX_QUEUES; n++)
		{
			struct core_sock *sock;
			int sock_queue;
			tx_response_t tx;

			sock_queue = atomic_read(&data->sock_queue[n]);
			smp_rmb();
			sock = data->sock[n];
			if (sock_queue != -1 && sock != NULL) {
				reg = true;
				tx = pfq_sk_queue_xmit(sock, sock_queue, data->cpu, &data->sock_queue[n]);
				total_sent += tx.ok;

				sparse_add(sock->stats,	  sent, tx.ok);
				sparse_add(sock->stats,   fail, tx.fail);
				sparse_add(global->percpu_stats,  sent, tx.ok);
				sparse_add(global->percpu_stats,  fail, tx.fail);
			}
		}

                if (kthread_should_stop())
                        break;

		pfq_relax();

#ifdef PFQ_DEBUG
		if (now != jiffies/(HZ*10)) {
			now = jiffies/(HZ*10);
			pfq_thread_ping("Tx", (struct pfq_thread_data *)data);
		}
#endif

		if (total_sent == 0)
			schedule();

		if (!reg)
			msleep(1);
	}

        printk(KERN_INFO "[PFQ] Tx[%d] thread stopped on cpu %d.\n", data->id, data->cpu);
	data->task = NULL;
        return 0;
}


int
pfq_bind_tx_thread(int tid, struct core_sock *sock, int sock_queue)
{
	struct pfq_thread_tx_data *thread_data;
	int n;

	if (tid >= global->tx_cpu_nr) {
		printk(KERN_INFO "[PFQ] Tx[%d] thread not available (%d Tx threads running)!\n", tid, global->tx_cpu_nr);
		return -ESRCH;
	}

	thread_data = &pfq_thread_tx_pool[tid];

	mutex_lock(&pfq_thread_tx_pool_lock);

	for(n = 0; n < Q_MAX_TX_QUEUES; n++)
	{
		if (atomic_read(&thread_data->sock_queue[n]) == -1)
			break;
	}

	if (n == Q_MAX_TX_QUEUES) {
		mutex_unlock(&pfq_thread_tx_pool_lock);
		printk(KERN_INFO "[PFQ] Tx[%d] thread busy (no queue available)!\n", tid);
		return -EBUSY;
	}

	thread_data->sock[n] = sock;
	smp_wmb();
	atomic_set(&thread_data->sock_queue[n], sock_queue);

        mutex_unlock(&pfq_thread_tx_pool_lock);
        printk(KERN_INFO "[PFQ] Tx[%d] thread bound to sock_id = %d, queue = %d...\n", tid, sock->id, sock_queue);
        return 0;
}


int
pfq_unbind_tx_thread(struct core_sock *sock)
{
	int n, i;
	mutex_lock(&pfq_thread_tx_pool_lock);

	for(n = 0; n < global->tx_cpu_nr; n++)
	{
		struct pfq_thread_tx_data *data = &pfq_thread_tx_pool[n];

		for(i = 0; i < Q_MAX_TX_QUEUES; i++)
		{
			if (atomic_read(&data->sock_queue[i]) != -1)
			{
				if (data->sock[i] == sock) {
					atomic_set(&data->sock_queue[i], -1);
					smp_wmb();
					msleep(Q_CORE_GRACE_PERIOD);
					data->sock[i] = NULL;
				}
			}
		}
	}

        mutex_unlock(&pfq_thread_tx_pool_lock);
        return 0;
}


int
pfq_start_tx_threads(void)
{
	int err = 0;

	if (global->tx_cpu_nr)
	{
		int n, node;
		printk(KERN_INFO "[PFQ] starting %d Tx thread(s)...\n", global->tx_cpu_nr);

		for(n = 0; n < global->tx_cpu_nr; n++)
		{
			struct pfq_thread_tx_data *data = &pfq_thread_tx_pool[n];

			node = global->tx_cpu[n] == -1 ? NUMA_NO_NODE : cpu_to_node(global->tx_cpu[n]);

			data->id = n;
			data->cpu = global->tx_cpu[n];
			data->task = kthread_create_on_node(pfq_tx_thread,
							    data, node,
							    "kpfq-Tx/%d", data->cpu);
			if (IS_ERR(data->task)) {
				printk(KERN_INFO "[PFQ] kernel_thread: create failed on cpu %d!\n",
				       data->cpu);
				err = PTR_ERR(data->task);
				data->task = NULL;
				return err;
			}

			kthread_bind(data->task, data->cpu);

			pr_devel("[PFQ] created Tx[%d] kthread on cpu %d...\n", data->id, data->cpu);

			wake_up_process(data->task);
		}
	}

	return err;
}


void
pfq_stop_tx_threads(void)
{
	if (global->tx_cpu_nr)
	{
		int n;

		printk(KERN_INFO "[PFQ] stopping %d Tx thread(s)...\n", global->tx_cpu_nr);

		for(n = 0; n < global->tx_cpu_nr; n++)
		{
			struct pfq_thread_tx_data *data = &pfq_thread_tx_pool[n];

			if (data->task)
			{
				int i;
				pr_devel("[PFQ stopping Tx[%d] thread@%p\n", data->id, data->task);

				kthread_stop(data->task);
				data->id   = -1;
				data->cpu  = -1;
				data->task = NULL;

				for(i=0; i < Q_MAX_TX_QUEUES; ++i)
				{
					atomic_set(&data->sock_queue[i], -1);
					data->sock[i] = NULL;
				}
			}
		}
	}
}


static int
pfq_rx_thread(void *_data)
{
	struct pfq_thread_rx_data *data = (struct pfq_thread_rx_data *)_data;


#ifdef PFQ_DEBUG
        int now = 0;
#endif

	if (data == NULL) {
		printk(KERN_INFO "[PFQ] Rx thread data error!\n");
		return -EPERM;
	}

	printk(KERN_INFO "[PFQ] Rx[%d] thread started on cpu %d.\n", data->id, data->cpu);

	__set_current_state(TASK_RUNNING);

        for(;;)
	{
		int i, n;

		/* poll the registered NAPI queues */

		for(i = 0; i < 1024; i++)
		{
			for(n = 0; n < data->napi_nr; n++)
			{
				struct core_percpu_data * cpudata = per_cpu_ptr(global->percpu_data, data->napi[n]);
				struct pfq_percpu_pool * pool = per_cpu_ptr(global->percpu_pool, data->napi[n]);
				pfq_receive_run(data->napi[n], 65536, cpudata, pool);
			}
		}

                if (unlikely(kthread_should_stop()))
                        break;

		pfq_relax();

		if (core_sock_counter() == 0)
			msleep(1);

#ifdef PFQ_DEBUG
		if (now != jiffies/(HZ*10)) {
			now = jiffies/(HZ*10);
			pfq_thread_ping("Rx", (struct pfq_thread_data *)data);
		}
#endif
	}

        printk(KERN_INFO "[PFQ] Rx[%d] thread stopped on cpu %d.\n", data->id, data->cpu);
	data->task = NULL;
        return 0;
}


int
pfq_start_rx_threads(void)
{
	int err = 0;

	if (global->rx_cpu_nr)
	{
		int napi_idx, napi_quota;
		int i, n, node;

		napi_quota = (global->napi_cpu_nr + global->rx_cpu_nr - 1)/global->rx_cpu_nr;

                napi_idx = 0;

		printk(KERN_INFO "[PFQ] starting %d Rx thread(s): napi quota %d...\n", global->rx_cpu_nr, napi_quota);

		for(n = 0; n < global->rx_cpu_nr; n++)
		{
			struct pfq_thread_rx_data *data = &pfq_thread_rx_pool[n];

			node = global->rx_cpu[n] == -1 ? NUMA_NO_NODE : cpu_to_node(global->rx_cpu[n]);

			data->id = n;
			data->cpu = global->rx_cpu[n];
			data->task = kthread_create_on_node(pfq_rx_thread,
							    data, node,
							    "kpfq-Rx/%d", data->cpu);
			if (IS_ERR(data->task)) {
				printk(KERN_INFO "[PFQ] kernel_thread: create failed on cpu %d!\n",
				       data->cpu);
				err = PTR_ERR(data->task);
				data->task = NULL;
				return err;
			}

			/* bind napi context to this thread */

			printk(KERN_INFO "[PFQ] creating Rx[%d] kthread on cpu %d...\n", data->id, data->cpu);

			for(i = 0; i < min(napi_quota, Q_MAX_RX_NAPI) &&
					napi_idx < global->napi_cpu_nr;  i++)
			{
				int napi_cpu = global->napi_cpu[napi_idx++];

				printk(KERN_INFO "[PFQ]    bound -> napi cpu %d\n", napi_cpu);

				data->napi[data->napi_nr++] = napi_cpu;

				/* disable napi processing on napi_cpu */
 				per_cpu_ptr(global->percpu_data, napi_cpu)->rx_napi = false;
			}

			kthread_bind(data->task, data->cpu);
			wake_up_process(data->task);
		}
	}

	return err;
}


void
pfq_stop_rx_threads(void)
{
	if (global->rx_cpu_nr)
	{
		int n;

		printk(KERN_INFO "[PFQ] stopping %d Rx thread(s)...\n", global->rx_cpu_nr);

		for(n = 0; n < global->rx_cpu_nr; n++)
		{
			struct pfq_thread_rx_data *data = &pfq_thread_rx_pool[n];

			if (data->task)
			{
				pr_devel("[PFQ stopping Rx[%d] thread@%p\n", data->id, data->task);

				kthread_stop(data->task);

				data->id   = -1;
				data->cpu  = -1;
				data->task = NULL;
			}
		}
	}
}


int
pfq_check_threads_affinity(void)
{
	bool inuse[Q_CORE_MAX_CPU] = {false};
	int i, cpu;

	/* check Rx thread affinity */

	for(i=0; i < global->rx_cpu_nr; ++i)
	{
		cpu = global->rx_cpu[i];
		if ( cpu < 0 || cpu >= num_online_cpus()) {
			printk(KERN_INFO "[PFQ] error: Rx[%d] thread bad affinity on cpu:%d!\n", i, cpu);
			return -EFAULT;
		}
		if (inuse[cpu]) {
			printk(KERN_INFO "[PFQ] error: Rx[%d] thread cpu:%d already in use!\n", i, cpu);
			return -EFAULT;
		}
		inuse[cpu] = true;
	}

	/* check Tx thread affinity */

	for(i=0; i < global->tx_cpu_nr; ++i)
	{
		cpu = global->tx_cpu[i];
		if (cpu < 0 || cpu >= num_online_cpus()) {
			printk(KERN_INFO "[PFQ] error: Tx[%d] thread bad affinity on cpu:%d!\n", i, cpu);
			return -EFAULT;
		}
		if (inuse[cpu]) {
			printk(KERN_INFO "[PFQ] error: Tx[%d] thread cpu:%d already in use!\n", i, cpu);
			return -EFAULT;
		}
		inuse[cpu] = true;
	}

	return 0;
}


int
pfq_check_napi_contexts(void)
{
	bool inuse[Q_CORE_MAX_CPU] = {false};
	int i, cpu;

	for(i=0; i < global->napi_cpu_nr; ++i)
	{
		cpu = global->napi_cpu[i];
		if ( cpu < 0 || cpu >= num_online_cpus()) {
			printk(KERN_INFO "[PFQ] error: Napi[%d] context: cpu %d bad value!\n", i, cpu);
			return -EFAULT;
		}
		if (inuse[cpu]) {
			printk(KERN_INFO "[PFQ] error: Napi[%d] context: cpu %d already bound!\n", i, cpu);
			return -EFAULT;
		}
		inuse[cpu] = true;
	}

	return 0;
}

