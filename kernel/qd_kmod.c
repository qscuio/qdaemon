/*
 * QDaemon Kernel Module - Main Entry
 * Generic Netlink interface for kernel-userspace communication
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <net/genetlink.h>

#define QD_KMOD_VERSION "1.0"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QDaemon");
MODULE_DESCRIPTION("QDaemon Kernel Module");
MODULE_VERSION(QD_KMOD_VERSION);

/* Netlink family name - must match userspace */
#define QD_GENL_NAME "QDAEMON"
#define QD_GENL_VERSION 1

/* Command definitions */
enum {
    QD_CMD_UNSPEC,
    QD_CMD_ECHO,
    QD_CMD_INFO,
    QD_CMD_NOTIFY,
    __QD_CMD_MAX,
};
#define QD_CMD_MAX (__QD_CMD_MAX - 1)

/* Attribute definitions */
enum {
    QD_ATTR_UNSPEC,
    QD_ATTR_MSG,
    QD_ATTR_DATA,
    QD_ATTR_STATUS,
    __QD_ATTR_MAX,
};
#define QD_ATTR_MAX (__QD_ATTR_MAX - 1)

/* Attribute policy */
static struct nla_policy qd_genl_policy[QD_ATTR_MAX + 1] = {
    [QD_ATTR_MSG] = { .type = NLA_NUL_STRING, .len = 256 },
    [QD_ATTR_DATA] = { .type = NLA_BINARY, .len = 4096 },
    [QD_ATTR_STATUS] = { .type = NLA_U32 },
};

/* Multicast group */
static struct genl_multicast_group qd_genl_mcgrps[] = {
    { .name = "events", },
};

/* Forward declarations */
static int qd_cmd_echo(struct sk_buff *skb, struct genl_info *info);
static int qd_cmd_info(struct sk_buff *skb, struct genl_info *info);
int qd_send_notify(const char *msg);

/* Operations */
static const struct genl_ops qd_genl_ops[] = {
    {
        .cmd = QD_CMD_ECHO,
        .flags = 0,
        .policy = qd_genl_policy,
        .doit = qd_cmd_echo,
    },
    {
        .cmd = QD_CMD_INFO,
        .flags = 0,
        .policy = qd_genl_policy,
        .doit = qd_cmd_info,
    },
};

/* Family definition */
static struct genl_family qd_genl_family = {
    .name = QD_GENL_NAME,
    .version = QD_GENL_VERSION,
    .maxattr = QD_ATTR_MAX,
    .ops = qd_genl_ops,
    .n_ops = ARRAY_SIZE(qd_genl_ops),
    .mcgrps = qd_genl_mcgrps,
    .n_mcgrps = ARRAY_SIZE(qd_genl_mcgrps),
    .module = THIS_MODULE,
};

/* Echo command - returns the same message */
static int qd_cmd_echo(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *reply;
    void *hdr;
    char *msg;

    if (!info->attrs[QD_ATTR_MSG])
        return -EINVAL;

    msg = nla_data(info->attrs[QD_ATTR_MSG]);

    reply = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!reply)
        return -ENOMEM;

    hdr = genlmsg_put(reply, info->snd_portid, info->snd_seq,
                      &qd_genl_family, 0, QD_CMD_ECHO);
    if (!hdr) {
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    if (nla_put_string(reply, QD_ATTR_MSG, msg)) {
        genlmsg_cancel(reply, hdr);
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    genlmsg_end(reply, hdr);
    return genlmsg_reply(reply, info);
}

/* Info command - returns kernel module info */
static int qd_cmd_info(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *reply;
    void *hdr;
    char msg[128];

    snprintf(msg, sizeof(msg), "QDaemon Kernel Module v%s", QD_KMOD_VERSION);

    reply = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!reply)
        return -ENOMEM;

    hdr = genlmsg_put(reply, info->snd_portid, info->snd_seq,
                      &qd_genl_family, 0, QD_CMD_INFO);
    if (!hdr) {
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    if (nla_put_string(reply, QD_ATTR_MSG, msg) ||
        nla_put_u32(reply, QD_ATTR_STATUS, 0)) {
        genlmsg_cancel(reply, hdr);
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    genlmsg_end(reply, hdr);
    return genlmsg_reply(reply, info);
}

/* Send multicast notification */
int qd_send_notify(const char *msg)
{
    struct sk_buff *skb;
    void *hdr;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!skb)
        return -ENOMEM;

    hdr = genlmsg_put(skb, 0, 0, &qd_genl_family, 0, QD_CMD_NOTIFY);
    if (!hdr) {
        nlmsg_free(skb);
        return -EMSGSIZE;
    }

    if (nla_put_string(skb, QD_ATTR_MSG, msg)) {
        genlmsg_cancel(skb, hdr);
        nlmsg_free(skb);
        return -EMSGSIZE;
    }

    genlmsg_end(skb, hdr);
    return genlmsg_multicast(&qd_genl_family, skb, 0, 0, GFP_KERNEL);
}
EXPORT_SYMBOL(qd_send_notify);

static int __init qd_kmod_init(void)
{
    int ret;

    ret = genl_register_family(&qd_genl_family);
    if (ret) {
        pr_err("qdaemon: Failed to register netlink family\n");
        return ret;
    }

    pr_info("qdaemon: Kernel module loaded\n");
    return 0;
}

static void __exit qd_kmod_exit(void)
{
    genl_unregister_family(&qd_genl_family);
    pr_info("qdaemon: Kernel module unloaded\n");
}

module_init(qd_kmod_init);
module_exit(qd_kmod_exit);
