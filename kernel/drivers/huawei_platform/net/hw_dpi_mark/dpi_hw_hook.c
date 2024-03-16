

#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/udp.h>
#include <linux/kernel.h>	   /* add for log */
#include <linux/ctype.h>		/* add for tolower */
#include <net/sock.h>

#include <linux/spinlock.h>	 /* add for spinlock */
#include <linux/netlink.h>	  /* add for thread */
#include <uapi/linux/netlink.h> /* add for netlink */
#include <linux/kthread.h>	  /* add for thread */
#include <linux/version.h>

#include "dpi_hw_hook.h"

#define REPORTCMD NETLINK_DOWNLOADEVENT_TO_USER

static int g_mark_tag = 0;	  /* the tag to decide whether to mark or not, default off */

/* WangZheRongYao Info */
static uint32_t g_tmgp_uid = 0;
static uint8_t g_tmgp_tp = 0;
static uint32_t g_tmgp_mark = 0;

/* netlink message format */
struct tag_hw_msg2knl {
	struct nlmsghdr hdr;
	int opt;
	char data[1];
};

/* DPI rule format */
typedef struct{
	dmr_match_type_t rule_type;
	union{
		uint8_t match_tp_val;
	}rule_body; /* rule body varies according to rule_type */
	uint32_t mark_num;
}dpi_rule_t;

/* dpi_mark_rule for one APP */
typedef struct{
	uid_t			   dmr_app_uid;
	dpi_rule_t		  dmr_rule;
}dpi_mark_rule_t;

static struct sock *g_hw_nlfd;
static unsigned int g_uspa_pid;

/* Add dpi rules to the hash list */
void add_dpi_rule(const char *data){
	dpi_mark_rule_t *p_dmr = (dpi_mark_rule_t *)data;

	/* Only support transportion protocol dpi for wangzherongyao till 2017/06/24,
	   TODO: add other dpi rules */
	switch(p_dmr->dmr_rule.rule_type){
		case DMR_MT_TP:
			g_tmgp_tp = p_dmr->dmr_rule.rule_body.match_tp_val;
			g_tmgp_uid = p_dmr->dmr_app_uid;
			g_tmgp_mark = p_dmr->dmr_rule.mark_num;
			break;
		default:
			break;
	}
}

/* process the cmd, opt not used currently */
static void _proc_cmd(int cmd, int opt, const char *data)
{
	switch(cmd){
		case NETLINK_SET_RULE_TO_KERNEL:
			add_dpi_rule(data);
			break;
		case NETLINK_START_MARK:
			g_mark_tag = 1;
			break;
		case NETLINK_STOP_MARK:
			g_mark_tag = 0;
			break;
		default:
			pr_info("hwdpi:kernel_hw_receive cmd=%d is wrong\n", cmd);	
	}
}

/* receive cmd for DPI netlink message */
static void kernel_ntl_receive(struct sk_buff *__skb)
{
	struct nlmsghdr *nlh;
	struct tag_hw_msg2knl *hmsg = NULL;
	struct sk_buff *skb;

	if (__skb == NULL){
		return;
	}

	skb = skb_get(__skb);
	if (skb && (skb->len >= NLMSG_HDRLEN)) {
		nlh = nlmsg_hdr(skb);
		if ((nlh->nlmsg_len >= sizeof(struct nlmsghdr)) &&
				(skb->len >= nlh->nlmsg_len)) {
			if (NETLINK_REG_TO_KERNEL == nlh->nlmsg_type)
				g_uspa_pid = nlh->nlmsg_pid;
			else if (NETLINK_UNREG_TO_KERNEL == nlh->nlmsg_type)
				g_uspa_pid = 0;
			else {
				hmsg = (struct tag_hw_msg2knl *)nlh;
				_proc_cmd(nlh->nlmsg_type, hmsg->opt, (char *)&(hmsg->data[0]));
			}
		}
	}
}

/* Initialize netlink socket, add hook function for netlink message receiving */
static void netlink_init(void)
{
	struct netlink_kernel_cfg hwcfg = {
		.input = kernel_ntl_receive,
	};
	g_hw_nlfd = netlink_kernel_create(&init_net, NETLINK_HW_DPI, &hwcfg);
	if (!g_hw_nlfd)
		pr_info("netlink_init failed NETLINK_HW_DPI\n");
}

/* mark the skb if it matched the rules */
void mark_skb_by_rule(struct sk_buff *skb, int tag){
	struct sock *sk = skb->sk;
	uid_t skb_uid;
	kuid_t kuid;
	struct iphdr *iph = ip_hdr(skb);

	/* tmgp_uid is not set */
	if (g_tmgp_uid == 0){
		return;
	}

	if (sk == NULL){
		return;
	}

    if( tag == 0 ){
        sk->sk_hwdpi_mark = sk->sk_hwdpi_mark & MR_RESET;
    }else{
        /* The socket is already marked, originally sk_hwdpi_mark is 0 */
        if (sk->sk_hwdpi_mark & MR_TMGP_2){
            skb->mark = g_tmgp_mark;
            return;
        }

        if ( sk->sk_hwdpi_mark & MR_MARKED ){
            return;
        }

        /* Only support qq tmgp tp identifying, TODO: other DPI rules */
        if ( iph && iph->protocol == g_tmgp_tp){
            kuid = sock_i_uid(sk);
            skb_uid = kuid.val;

            if ( skb_uid == g_tmgp_uid ){
                sk->sk_hwdpi_mark = sk->sk_hwdpi_mark | MR_TMGP_2;
                skb->mark = g_tmgp_mark;
                return;
            }
        }

        sk->sk_hwdpi_mark = sk->sk_hwdpi_mark | MR_MARKED; /* This socket is not tmgp gaming udp */
    }
}

static unsigned int dpimark_hook_localout(const struct nf_hook_ops *ops,
					 struct sk_buff *skb,
					 const struct nf_hook_state *state)
{
	/* match the packet for optimization */
    if (skb){
         mark_skb_by_rule(skb,g_mark_tag);
    }
	return NF_ACCEPT;
}

static struct nf_hook_ops net_hooks[] = {
	{
		.hook		= dpimark_hook_localout,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0))
		.owner		  = THIS_MODULE,
#endif
		.pf		= PF_INET,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= NF_IP_PRI_FILTER - 1,
	},
};

static int __init nf_init(void)
{
	int ret = 0;

	/* Add a hook point on NF_INET_LOCAL_OUT, where we can get all the packets generated by local APP */
	ret = nf_register_hooks(net_hooks, ARRAY_SIZE(net_hooks));
	if (ret) {
		pr_info("nf_init ret=%d  ", ret);
		return -1;
	}

	/* Initialize the netlink connection */
	netlink_init();
	
	pr_info("dpi_hw_hook_init success\n");

	return 0;
}

static void __exit nf_exit(void)
{
	nf_unregister_hooks(net_hooks, ARRAY_SIZE(net_hooks));
}

module_init(nf_init);
module_exit(nf_exit);

MODULE_LICENSE("Dual BSD");
MODULE_AUTHOR("s00399850");
MODULE_DESCRIPTION("HW DPI MARK NF_HOOK");
MODULE_VERSION("1.0.1");
MODULE_ALIAS("HW LWDPI 01");
