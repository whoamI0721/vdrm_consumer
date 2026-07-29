#include <compiler.h>
#include <kpmodule.h>
#include <log.h>
#include <linux/string.h>
#include <common.h>
#include <kallsyms.h>
#include <hook.h>
#include <kputils.h>
#include <baselib.h>
#include <stddef.h>
#include <linux/errno.h>
#include <asm/current.h>
#include <asm/thread_info.h>

#ifndef IS_ERR
#define IS_ERR(x) ((unsigned long)(x) >= (unsigned long)-4095)
#endif
#ifndef PTR_ERR
#define PTR_ERR(x) ((long)(x))
#endif

KPM_NAME("vdrm")
KPM_VERSION("0.4.0")
KPM_LICENSE("GPL v2")
KPM_AUTHOR("opencode")
KPM_DESCRIPTION("Virtual DRM for chroot containers + zero-copy display")

static int (*compat_copy_from_user)(void *, const void __user *, int);
static int (*k_copy_to_user)(void __user *, const void *, int);

struct drm_file;
struct drm_gem_object;
struct dma_buf;
struct drm_minor;
struct file;
static struct drm_gem_object *(*k_drm_gem_object_lookup)(struct drm_file *, unsigned int);
static struct dma_buf *(*k_dma_buf_get)(int);
struct drm_device;
static int (*k_dma_buf_fd)(struct dma_buf *, int);
static void (*k_dma_buf_put)(struct dma_buf *);
static long (*k_strncpy_from_user)(char *, const char __user *, long);
static unsigned int (*k_msleep_interruptible)(unsigned int);
static int (*k_wake_up_process)(struct task_struct *);
static struct task_struct *(*k_find_task_by_vpid)(pid_t);
static pid_t (*k_task_pid_nr)(struct task_struct *, int, void *);
static struct pid_namespace *(*k_task_active_pid_ns)(struct task_struct *);
static void (*k_rcu_read_lock)(void);
static void (*k_rcu_read_unlock)(void);

/* Audio pipe helpers */
static int (*k_do_pipe_flags)(int *, int);
static struct file *(*k_close_fd_get_file)(unsigned int);
static int (*k_get_unused_fd_flags)(unsigned);
static void (*k_fd_install)(unsigned int, struct file *);
static void (*k_fput)(struct file *);

static inline int has_signal(void) {
    return *(volatile unsigned long *)current & _TIF_SIGPENDING;
}

#define FILE_PRIV_OFF 0xD8
#define OBJ_IDR_OFF   0x40  /* drm_file->object_idr (correct, confirmed by spy) */
#define MINOR_OFF     0x38
#define MINOR_DEV_OFF 0x10

#define DRM_MODE_PROP_ENUM      8
#define DRM_MODE_PROP_BLOB      16
#define DRM_MODE_PROP_IMMUTABLE 4
#define DRM_PROP_NAME_LEN       32

#define PROP_DPMS       1001
#define PROP_LINK_STATUS 1002

struct prop_enum_s { unsigned long long v; char n[32]; };

#define GETCRTC_NR   0xA1
#define SETCRTC_NR   0xA2
#define GETENC_NR    0xA6
#define GETCONN_NR   0xA7
#define ADDFB2_NR    0xB8
#define FLIP_NR      0xB0
#define ATOMIC_NR    0xBC
#define CREATE_DUMB_NR 0xB2

#define MAX_FBS 256
struct fb_entry {
    unsigned int fb_id;
    unsigned int w, h, fmt, fl;
    unsigned int handles[4];
    unsigned int pitches[4];
    unsigned int offsets[4];
    unsigned long long mods[4];
    struct drm_gem_object *objs[4];
};
static struct fb_entry fb_table[MAX_FBS];
static int fb_count;

#define MAGIC_PATH "/vdrm_magic"

#define TASK_FS_OFF    0x870
#define FS_ROOT_OFF    0x18
#define DENTRY_IN_PATH 0x08

static struct dentry *container_root_dentry;
static int vdrm_enabled;

static void *orig_sys_chroot;
static void *orig_drm_ioctl;
static void *orig_sys_openat;

/* Event types */
#define EV_KEY    1
#define EV_BUTTON 2
#define EV_MOTION 3
#define EV_SCROLL 4

#define EV_RING_SIZE 256
struct vdrm_event { int type, code, value, x, y; };
static struct {
    struct vdrm_event buf[EV_RING_SIZE];
    volatile int head, tail;
} ev_ring;

/* O_NONBLOCK from <asm-generic/fcntl.h> */
#define VDRM_O_NONBLOCK 0x800

/* Audio pipe file transfer (do_pipe_flags + close_fd_get_file) */
static struct file *audio_play_file;
static struct file *audio_cap_file;

/* APK zero-copy state */
#define APK_BUF_MAX 6
static struct dma_buf *apk_bufs[APK_BUF_MAX];
static int apk_buf_count;
static int apk_pending_idx;
static volatile int apk_flip_done;
static pid_t apk_present_pid;
static struct drm_device *v_drm_dev;
static int v_drm_dev_captured;

static volatile int vdrm_lock;
__attribute__((always_inline)) static inline void vdrm_spin_lock(volatile int *l) {
    unsigned int tmp;
    asm volatile(
        "1: ldxr %w0, [%1]\n"
        "   cbnz %w0, 1b\n"
        "   stxr %w0, %w2, [%1]\n"
        "   cbnz %w0, 1b\n"
        : "=&r"(tmp) : "r"(l), "r"(1) : "memory");
}
__attribute__((always_inline)) static inline void vdrm_spin_unlock(volatile int *l) {
    asm volatile("stlr wzr, [%0]" : : "r"(l) : "memory");
}


static struct dentry *get_root_dentry(struct task_struct *task)
{
    unsigned long fs_addr = *(unsigned long *)((char *)task + TASK_FS_OFF);
    if (!fs_addr) return NULL;
    return *(struct dentry **)(fs_addr + FS_ROOT_OFF + DENTRY_IN_PATH);
}

static int in_container(void)
{
    if (!container_root_dentry) return 0;
    return get_root_dentry(current) == container_root_dentry;
}

static void push_event(int type, int code, int value, int x, int y)
{
    vdrm_spin_lock(&vdrm_lock);
    if (ev_ring.head - ev_ring.tail < EV_RING_SIZE) {
        int i = ev_ring.head % EV_RING_SIZE;
        ev_ring.buf[i].type = type;
        ev_ring.buf[i].code = code;
        ev_ring.buf[i].value = value;
        ev_ring.buf[i].x = x;
        ev_ring.buf[i].y = y;
        ev_ring.head++;
    }
    vdrm_spin_unlock(&vdrm_lock);
}

static void after_chroot(hook_fargs1_t *args, void *udata)
{
    if ((long)args->ret < 0) return;
    container_root_dentry = get_root_dentry(current);
    logki("vdrm: container root dentry captured\n");
}

/* Simple integer parser from string */
static int parse_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* ---- Virtualized IOCTL handlers ---- */

static int try_getresources(unsigned long arg)
{
    unsigned char user[64];
    int cfu = compat_copy_from_user(user, (void *)arg, 64);
    if (cfu) return -EFAULT;

    unsigned int *uc = (unsigned int *)(user + 32);
    int dp = (uc[1] > 0 || uc[2] > 0 || uc[3] > 0);

    if (dp) {
        unsigned long long *pu = (unsigned long long *)user;
        unsigned int id = 1;
        if (pu[1] && k_copy_to_user((void *)(unsigned long)pu[1], &id, 4)) return -EFAULT;
        if (pu[2] && k_copy_to_user((void *)(unsigned long)pu[2], &id, 4)) return -EFAULT;
        if (pu[3] && k_copy_to_user((void *)(unsigned long)pu[3], &id, 4)) return -EFAULT;
    }

    if (k_copy_to_user((void *)arg, user, 32)) return -EFAULT;

    unsigned int zero = 0, one = 1;
    if (k_copy_to_user((void *)(arg + 32), &zero, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 36), &one, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 40), &one, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 44), &one, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 48), &zero, 4)) return -EFAULT;
    unsigned int mw = 4096;
    if (k_copy_to_user((void *)(arg + 52), &mw, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 56), &zero, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 60), &mw, 4)) return -EFAULT;
    return 0;
}

static int try_getconn(unsigned long arg)
{
    unsigned char in[80];
    if (compat_copy_from_user(in, (void *)arg, 80)) return -EFAULT;
    unsigned int cid = 0;
    compat_copy_from_user(&cid, (void *)(arg + 48), 4);

    if (cid == 69) {
        unsigned int z[] = {0,0,0,0,0,0,0,0,0,0,0,0};
        if (k_copy_to_user((void *)(arg + 32), z, sizeof(z))) return -EFAULT;
        return 0;
    }

    unsigned int v[] = {1,0,1,1,1,1,1,1,0,0,0,0};
    if (k_copy_to_user((void *)(arg + 32), v, sizeof(v))) return -EFAULT;

    unsigned long long mod_p = *(unsigned long long *)(in + 8);
    if (mod_p) {
        unsigned char mode[68];
        memset(mode, 0, 68);
        unsigned int *mc = (unsigned int *)mode;
        unsigned short *ms = (unsigned short *)(mode + 4);
        mc[0] = 148500;
        ms[0] = 1920; ms[1] = 2008; ms[2] = 2052; ms[3] = 2200;
        ms[5] = 1080; ms[6] = 1084; ms[7] = 1089; ms[8] = 1125;
        mc[6] = 60000; mc[7] = 5; mc[8] = 257;
        mode[36]='v';mode[37]='d';mode[38]='r';mode[39]='m';
        if (k_copy_to_user((void *)(unsigned long)mod_p, mode, 68)) return -EFAULT;
    }
    unsigned long long enc_p = *(unsigned long long *)(in);
    if (enc_p) {
        unsigned int eid = 1;
        if (k_copy_to_user((void *)(unsigned long)enc_p, &eid, 4)) return -EFAULT;
    }
    return 0;
}

static int try_getcrtc(unsigned long arg)
{
    unsigned char in[104];
    if (compat_copy_from_user(in, (void *)arg, 104)) return -EFAULT;
    unsigned int cid = 0;
    compat_copy_from_user(&cid, (void *)(arg + 12), 4);

    if (cid == 69) {
        unsigned char z[104];
        memset(z, 0, 104);
        if (k_copy_to_user((void *)arg, z, 104)) return -EFAULT;
        return 0;
    }

    unsigned int b[26];
    memset(b, 0, sizeof(b));
    b[3] = cid;
    if (k_copy_to_user((void *)arg, b, 104)) return -EFAULT;
    return 0;
}

static int try_getencoder(unsigned long arg)
{
    unsigned int eid;
    compat_copy_from_user(&eid, (void *)arg, 4);

    if (eid == 69) {
        unsigned char z[20];
        memset(z, 0, 20);
        if (k_copy_to_user((void *)arg, z, 20)) return -EFAULT;
        return 0;
    }

    unsigned int v[] = {1, 1, 1, 1, 0};
    if (k_copy_to_user((void *)arg, v, sizeof(v))) return -EFAULT;
    return 0;
}

static int try_getprops(hook_fargs3_t *args, unsigned long arg)
{
    unsigned int obj_id = 0, obj_type = 0;
    compat_copy_from_user(&obj_id, (void *)(arg + 20), 4);
    compat_copy_from_user(&obj_type, (void *)(arg + 24), 4);

    if (!(obj_id == 1 && (obj_type == 0xCCCCCCCC || obj_type == 0xC0C0C0C0))) {
        args->skip_origin = 0;
        return 0;
    }

    unsigned int count;
    compat_copy_from_user(&count, (void *)(arg + 16), 4);

    if (count == 0) {
        unsigned int c = 2;
        if (k_copy_to_user((void *)(arg + 16), &c, 4)) return -EFAULT;
        return 0;
    }

    unsigned long long pptr;
    compat_copy_from_user((void *)&pptr, (void *)arg, 8);
    unsigned long long vptr;
    compat_copy_from_user((void *)&vptr, (void *)(arg + 8), 8);

    unsigned int prop_ids[] = {PROP_DPMS, PROP_LINK_STATUS};
    unsigned long long prop_vals[] = {0, 0};

    if (pptr) {
        if (k_copy_to_user((void *)(unsigned long)pptr, prop_ids, 8)) return -EFAULT;
    }
    if (vptr) {
        if (k_copy_to_user((void *)(unsigned long)vptr, prop_vals, 16)) return -EFAULT;
    }
    return 0;
}
static int try_atomic(unsigned long arg) { return 0; }
static int try_setcrtc(unsigned long arg) { return 0; }

static int try_getprop(hook_fargs3_t *args, unsigned long arg)
{
    unsigned int prop_id;
    compat_copy_from_user(&prop_id, (void *)(arg + 16), 4);

    unsigned int flags, num_enums;
    const char *name;

    if (prop_id == PROP_DPMS) { flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE; num_enums = 4; name = "DPMS"; }
    else if (prop_id == PROP_LINK_STATUS) { flags = DRM_MODE_PROP_ENUM; num_enums = 2; name = "link-status"; }
    else { args->skip_origin = 0; return 0; }

    if (k_copy_to_user((void *)(arg + 16), &prop_id, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 20), &flags, 4)) return -EFAULT;
    if (k_copy_to_user((void *)(arg + 24), name, 32)) return -EFAULT;

    {
        unsigned int cv = 0, ce = num_enums;
        if (k_copy_to_user((void *)(arg + 56), &cv, 4)) return -EFAULT;
        if (k_copy_to_user((void *)(arg + 60), &ce, 4)) return -EFAULT;
    }

    unsigned long long ev_ptr;
    compat_copy_from_user((void *)&ev_ptr, (void *)(arg + 8), 8);
    if (ev_ptr && num_enums) {
        struct prop_enum_s enums[4];
        memset(enums, 0, sizeof(enums));
        unsigned int i;
        for (i = 0; i < num_enums && i < 4; i++) enums[i].v = i;
        if (prop_id == PROP_DPMS) {
            char *e_names[] = {"On", "Standby", "Suspend", "Off"};
            for (i = 0; i < num_enums && i < 4; i++) {
                unsigned int j;
                for (j = 0; e_names[i][j] && j < 31; j++) enums[i].n[j] = e_names[i][j];
            }
        } else if (prop_id == PROP_LINK_STATUS) {
            char *e_names[] = {"Good", "Bad"};
            for (i = 0; i < num_enums && i < 2; i++) {
                unsigned int j;
                for (j = 0; e_names[i][j] && j < 31; j++) enums[i].n[j] = e_names[i][j];
            }
        }
        if (k_copy_to_user((void *)(unsigned long)ev_ptr, enums, num_enums * sizeof(struct prop_enum_s))) return -EFAULT;
    }
    return 0;
}

static int try_getblob(unsigned long arg)
{
    unsigned long long data_ptr;
    compat_copy_from_user((void *)&data_ptr, (void *)arg, 8);
    unsigned int blob_id;
    compat_copy_from_user(&blob_id, (void *)(arg + 8), 4);
    unsigned int length;
    compat_copy_from_user(&length, (void *)(arg + 12), 4);

    unsigned int out_len = 0;
    if (blob_id == 200) {
        out_len = 0;
        if (k_copy_to_user((void *)(arg + 12), &out_len, 4)) return -EFAULT;
        return 0;
    }
    return -ENOENT;
}

static unsigned int next_fb_id(void)
{
    static unsigned int counter;
    return (counter++) | 0xF0000000;
}

static int try_addfb2(void *filp, unsigned long arg)
{
    struct {
        unsigned int id, w, h, fmt, fl;
        unsigned int hd[4], p[4], o[4];
        unsigned long long mod[4];
    } f;
    if (compat_copy_from_user(&f, (void *)arg, sizeof(f))) return -EFAULT;
    if (fb_count >= MAX_FBS) return -ENOMEM;

    struct drm_file *drm_file = *(struct drm_file **)((char *)filp + FILE_PRIV_OFF);
    if (!drm_file) return -EINVAL;

    unsigned int fake_id = next_fb_id();
    struct fb_entry *fb = &fb_table[fb_count++];
    fb->fb_id = fake_id;
    fb->w = f.w; fb->h = f.h; fb->fmt = f.fmt; fb->fl = f.fl;
    for (int i = 0; i < 4; i++) {
        fb->handles[i] = f.hd[i];
        fb->pitches[i] = f.p[i];
        fb->offsets[i] = f.o[i];
        fb->mods[i] = f.mod[i];
        fb->objs[i] = NULL;
        if (f.hd[i]) {
            fb->objs[i] = k_drm_gem_object_lookup(drm_file, f.hd[i]);
            if (!fb->objs[i])
                logkw("vdrm: addfb2 lookup failed handle=%u plane=%d\n", f.hd[i], i);
        }
    }

    f.id = fake_id;
    if (k_copy_to_user((void *)arg, &f, sizeof(f))) return -EFAULT;
    logki("vdrm: addfb2 fb=0x%x w=%u h=%u fmt=0x%x handle0=%u\n",
          fake_id, f.w, f.h, f.fmt, f.hd[0]);
    return 0;
}

static int try_flip(unsigned long arg)
{
    struct {
        unsigned int cid, fb, fl, rsv;
        unsigned long long ud;
    } f;
    if (compat_copy_from_user(&f, (void *)arg, sizeof(f))) return -EFAULT;

    struct fb_entry *fb = NULL;
    for (int i = 0; i < fb_count; i++) {
        if (fb_table[i].fb_id == f.fb) { fb = &fb_table[i]; break; }
    }
    if (!fb) {
        logkw("vdrm: flip unknown fb=0x%x\n", f.fb);
    } else {
        logki("vdrm: flip fb=0x%x handled\n", f.fb);
    }

    struct task_struct *wtask = NULL;
    vdrm_spin_lock(&vdrm_lock);
    logki("vdrm: try_flip present_pid=%d\n", apk_present_pid);
    if (k_rcu_read_lock) k_rcu_read_lock();
    if (apk_present_pid) {
        wtask = k_find_task_by_vpid(apk_present_pid);
        logki("vdrm: try_flip find_task_by_vpid(%d)=%p\n", apk_present_pid, wtask);
        if (wtask) {
            apk_flip_done = 1;
            apk_present_pid = 0;
        }
    }
    vdrm_spin_unlock(&vdrm_lock);
    if (wtask) {
        k_wake_up_process(wtask);
    }
    if (k_rcu_read_unlock) k_rcu_read_unlock();
    return 0;
}

/* ---- CREATE_DUMB after-hook: no-op (zero-copy via dmabuf_fd/ path) ---- */
/* ---- IOCTL dispatch ---- */
static void before_ioctl(hook_fargs3_t *args, void *udata)
{
    unsigned int cmd = (unsigned int)args->arg1;
    unsigned long arg = args->arg2;
    int nr = cmd & 0xff;
    int ret = 0;

    if (!vdrm_enabled) return;
    if (!in_container()) return;
    if (nr > 0xbf) return;

    /* Capture drm_device from first chroot ioctl (needed for submit validation) */
    if (!v_drm_dev_captured) {
        void *filp = (void *)args->arg0;
        struct drm_file *drm_file = *(struct drm_file **)((char *)filp + FILE_PRIV_OFF);
        if (drm_file) {
            struct drm_minor *minor = *(struct drm_minor **)((char *)drm_file + MINOR_OFF);
            if (minor) {
                v_drm_dev = *(struct drm_device **)((char *)minor + MINOR_DEV_OFF);
                v_drm_dev_captured = 1;
                /* Also peek at drm_device->dev (first field) */
                unsigned long dev_ptr_val = 0;
                if (v_drm_dev)
                    dev_ptr_val = *(unsigned long *)v_drm_dev;
                logki("vdrm: drm_device captured val=%lx dev_ptr=%lx (off=0x%x)\n",
                      (unsigned long)v_drm_dev, dev_ptr_val, MINOR_DEV_OFF);
            }
        }
    }

    args->skip_origin = 1;

    switch (nr) {
    case 0xA0:       ret = try_getresources(arg); break;
    case GETCRTC_NR: ret = try_getcrtc(arg); break;
    case GETENC_NR:  ret = try_getencoder(arg); break;
    case GETCONN_NR: ret = try_getconn(arg); break;
    case SETCRTC_NR: ret = try_setcrtc(arg); break;
    case ADDFB2_NR:  ret = try_addfb2((void *)args->arg0, arg); break;
    case 0xB9:
        ret = try_getprops(args, arg);
        if (!args->skip_origin) return;
        break;
    case 0xAA:
        ret = try_getprop(args, arg);
        if (!args->skip_origin) return;
        break;
    case 0xAC:       ret = try_getblob(arg); break;
    case FLIP_NR:    ret = try_flip(arg); break;
    case ATOMIC_NR:  ret = try_atomic(arg); break;
    case CREATE_DUMB_NR: args->skip_origin = 0; return; /* pass through to kernel, no VDRM handling */
    case 0xBE: {
        /* VDRM_GET_EVENT — chroot reads input event from ring buffer */
        while (ev_ring.head == ev_ring.tail) {
            if (has_signal()) { ret = -ERESTARTSYS; break; }
            k_msleep_interruptible(1);
        }
        if (ret == 0) {
            struct vdrm_event ev;
            vdrm_spin_lock(&vdrm_lock);
            ev = ev_ring.buf[ev_ring.tail % EV_RING_SIZE];
            ev_ring.tail++;
            vdrm_spin_unlock(&vdrm_lock);
            if (k_copy_to_user((void *)arg, &ev, sizeof(ev)))
                ret = -EFAULT;
        }
        break;
    }
    default:
        args->skip_origin = 0;
        return;
    }

    if (ret < 0)
        logkw("vdrm: ioctl nr=0x%x ret=%d\n", nr, ret);
    args->ret = (unsigned long)(long)ret;
}

/* ---- openat hook (APK ↔ KPM via path) ---- */
static void before_openat(hook_fargs1_t *args, void *udata)
{
    void *regs = (void *)args->arg0;
    unsigned long *reg_array = (unsigned long *)regs;
    const char __user *filename = (const char __user *)reg_array[1];
    int flags = (int)reg_array[2];

    if (!filename) return;

    char buf[128];
    long ret = k_strncpy_from_user(buf, filename, sizeof(buf) - 1);
    if (ret <= 0) return;
    buf[ret] = 0;

    int mlen = strlen(MAGIC_PATH);
    if (strncmp(buf, MAGIC_PATH, mlen) != 0) return;

    args->skip_origin = 1;

    /* Plain "/vdrm_magic" — handoff mode: deprecated */
    if (buf[mlen] == 0) { args->ret = (unsigned long)-ENOENT; return; }
    if (buf[mlen] != '/') { args->ret = (unsigned long)-EINVAL; return; }

    const char *cmd = buf + mlen + 1;

    /* /vdrm_magic/submit/<dmabuf_fd> — APK registers a dma_buf (non-container only) */
    if (strncmp(cmd, "submit/", 7) == 0) {
        if (in_container()) { args->ret = (unsigned long)-EPERM; return; }
        int fd = parse_int(cmd + 7);
        if (fd < 0) { logkw("vdrm: submit invalid fd=%d\n", fd); args->ret = (unsigned long)-EINVAL; return; }

        struct dma_buf *dmabuf = k_dma_buf_get(fd);
        if (IS_ERR(dmabuf)) {
            logkw("vdrm: dma_buf_get(%d) failed: %ld\n", fd, PTR_ERR(dmabuf));
            args->ret = (unsigned long)PTR_ERR(dmabuf);
            return;
        }

        logki("vdrm: submit fd=%d dmabuf=%p idx=%d\n", fd, dmabuf, apk_buf_count);

        vdrm_spin_lock(&vdrm_lock);
        if (apk_buf_count >= APK_BUF_MAX) {
            vdrm_spin_unlock(&vdrm_lock);
            logkw("vdrm: apk buf full\n");
            k_dma_buf_put(dmabuf);
            args->ret = (unsigned long)-ENOSPC;
            return;
        }

        apk_bufs[apk_buf_count++] = dmabuf;  /* store dma_buf, chroot imports via dmabuf_fd/openat */
        logki("vdrm: apk submit fd=%d idx=%d\n", fd, apk_buf_count - 1);
        args->ret = (unsigned long)(apk_buf_count - 1);
        vdrm_spin_unlock(&vdrm_lock);
        return;
    }

    /* /vdrm_magic/present/<idx> — APK waits for compositor flip */
    if (strncmp(cmd, "present/", 8) == 0) {
        int idx = parse_int(cmd + 8);

        vdrm_spin_lock(&vdrm_lock);
        if (idx < 0 || idx >= apk_buf_count) { vdrm_spin_unlock(&vdrm_lock); logkw("vdrm: present invalid idx=%d\n", idx); args->ret = (unsigned long)-EINVAL; return; }

        apk_pending_idx = idx;
        apk_flip_done = 0;
        apk_present_pid = k_task_pid_nr(current, 0, k_task_active_pid_ns ? k_task_active_pid_ns(current) : NULL);
        vdrm_spin_unlock(&vdrm_lock);
        logki("vdrm: present enter idx=%d pid=%d\n", idx, apk_present_pid);

        /* Try O_NONBLOCK: return immediately for polling use */
        if (flags & 0x800) { /* O_NONBLOCK */
            vdrm_spin_lock(&vdrm_lock);
            apk_present_pid = 0;
            vdrm_spin_unlock(&vdrm_lock);
            logki("vdrm: present NONBLOCK return idx=%d\n", idx);
            args->ret = (unsigned long)-EAGAIN;
            return;
        }

        /* Block until flip completes (msleep_interruptible sets TASK_INTERRUPTIBLE) */
        {
            int __spin;
            for (__spin = 0; ; __spin++) {
                bool flip_done;
                vdrm_spin_lock(&vdrm_lock);
                flip_done = apk_flip_done;
                vdrm_spin_unlock(&vdrm_lock);
                if (flip_done) {
                    logki("vdrm: present flip_done idx=%d spins=%d\n", idx, __spin);
                    break;
                }
                /* Escape hatch: ~3000 spins = ~30s timeout → prevent soft lockup panic */
                if (__spin > 3000) {
                    logkw("vdrm: present TIMEOUT idx=%d spins=%d, forcing exit\n", idx, __spin);
                    vdrm_spin_lock(&vdrm_lock);
                    apk_present_pid = 0;
                    vdrm_spin_unlock(&vdrm_lock);
                    args->ret = (unsigned long)-ETIMEDOUT;
                    return;
                }
                if (has_signal()) {
                    logki("vdrm: present SIGNAL exit idx=%d spins=%d\n", idx, __spin);
                    vdrm_spin_lock(&vdrm_lock);
                    apk_present_pid = 0;
                    vdrm_spin_unlock(&vdrm_lock);
                    args->ret = (unsigned long)-ERESTARTSYS;
                    return;
                }
                k_msleep_interruptible(10);
            }
        }

        vdrm_spin_lock(&vdrm_lock);
        apk_present_pid = 0;
        vdrm_spin_unlock(&vdrm_lock);
        logki("vdrm: present done idx=%d\n", idx);
        args->ret = 0;
        return;
    }

    /* /vdrm_magic/dmabuf_fd/<idx> — chroot gets the dma-buf fd for EGL import */
    if (strncmp(cmd, "dmabuf_fd/", 10) == 0) {
        if (!in_container()) { args->ret = (unsigned long)-EPERM; return; }
        int idx = parse_int(cmd + 10);
        vdrm_spin_lock(&vdrm_lock);
        if (idx < 0 || idx >= apk_buf_count || !apk_bufs[idx]) {
            vdrm_spin_unlock(&vdrm_lock);
            args->ret = (unsigned long)-ENOENT;
            return;
        }
        struct dma_buf *dmabuf = apk_bufs[idx];
        vdrm_spin_unlock(&vdrm_lock);
        int new_fd = k_dma_buf_fd(dmabuf, 0x80000); /* O_CLOEXEC */
        if (new_fd < 0) {
            logkw("vdrm: dmabuf_fd failed idx=%d err=%d\n", idx, new_fd);
            args->ret = (unsigned long)-EIO;
            return;
        }
        logki("vdrm: dmabuf_fd idx=%d fd=%d\n", idx, new_fd);
        args->ret = (unsigned long)new_fd;
        return;
    }

    /* /vdrm_magic/flip/<idx> — chroot signals flip done, wakes APK */
    if (strncmp(cmd, "flip/", 5) == 0) {
        if (!in_container()) { args->ret = (unsigned long)-EPERM; return; }
        int idx = parse_int(cmd + 5);
        struct task_struct *task = NULL;
        vdrm_spin_lock(&vdrm_lock);
        logki("vdrm: flip enter idx=%d pending_idx=%d present_pid=%d\n",
              idx, apk_pending_idx, apk_present_pid);
        if (k_rcu_read_lock) k_rcu_read_lock();
        if (idx == apk_pending_idx && apk_present_pid) {
            task = k_find_task_by_vpid(apk_present_pid);
            logki("vdrm: flip find_task_by_vpid(%d)=%p\n", apk_present_pid, task);
            if (task) {
                apk_flip_done = 1;
                apk_present_pid = 0;
            }
        }
        vdrm_spin_unlock(&vdrm_lock);
        if (task) {
            k_wake_up_process(task);
            logki("vdrm: flip idx=%d done\n", idx);
        } else {
            logkw("vdrm: flip idx=%d mismatch pending_idx=%d\n", idx, apk_pending_idx);
        }
        if (k_rcu_read_unlock) k_rcu_read_unlock();
        args->ret = 0;
        return;
    }

    /* ---- EV path: APK writes input events ---- */
    if (strncmp(cmd, "ev/", 3) == 0) {
        const char *p = cmd + 3;

        if (strncmp(p, "key/", 4) == 0) {
            p += 4;
            int action = -1;
            if (strncmp(p, "down/", 5) == 0) { action = 1; p += 5; }
            else if (strncmp(p, "up/", 3) == 0) { action = 0; p += 3; }
            if (action < 0) { args->ret = (unsigned long)-EINVAL; return; }
            int keycode = parse_int(p);
            push_event(EV_KEY, keycode, action, 0, 0);
            args->ret = 0;
            return;
        }

        if (strncmp(p, "btn/", 4) == 0) {
            p += 4;
            int btn = -1;
            if (strncmp(p, "left/", 5) == 0) { btn = 0x110; p += 5; }
            else if (strncmp(p, "right/", 6) == 0) { btn = 0x111; p += 6; }
            else if (strncmp(p, "middle/", 7) == 0) { btn = 0x112; p += 7; }
            if (btn < 0) { args->ret = (unsigned long)-EINVAL; return; }
            int pressed = parse_int(p);
            push_event(EV_BUTTON, btn, pressed, 0, 0);
            args->ret = 0;
            return;
        }

        if (strncmp(p, "motion/", 7) == 0) {
            p += 7;
            if (strncmp(p, "dx/", 3) != 0) { args->ret = (unsigned long)-EINVAL; return; }
            p += 3;
            int dx = parse_int(p); while (*p && *p != '/') p++;
            if (*p != '/') { args->ret = (unsigned long)-EINVAL; return; }
            p++;
            if (strncmp(p, "dy/", 3) != 0) { args->ret = (unsigned long)-EINVAL; return; }
            p += 3;
            int dy = parse_int(p);
            push_event(EV_MOTION, 0, 0, dx, dy);
            args->ret = 0;
            return;
        }

        if (strncmp(p, "scroll/", 7) == 0) {
            p += 7;
            int axis = parse_int(p); while (*p && *p != '/') p++;
            if (*p != '/') { args->ret = (unsigned long)-EINVAL; return; }
            p++;
            int val = parse_int(p);
            push_event(EV_SCROLL, axis, val, 0, 0);
            args->ret = 0;
            return;
        }

        args->ret = (unsigned long)-EINVAL;
        return;
    }

    /* ---- Audio pipe paths ---- */
    if (strncmp(cmd, "audio/play", 10) == 0 && cmd[10] == 0) {
        /* APK: create playback pipe, return read end */
        int fds[2];
        int pipe_flags = flags & VDRM_O_NONBLOCK;
        if (k_do_pipe_flags(fds, pipe_flags) < 0) { args->ret = (unsigned long)-ENOMEM; return; }
        audio_play_file = k_close_fd_get_file(fds[1]);
        logki("vdrm: audio play pipe created\n");
        args->ret = (unsigned long)fds[0];
        return;
    }

    if (strncmp(cmd, "audio/cap", 9) == 0 && cmd[9] == 0) {
        int fds[2];
        int pipe_flags = flags & VDRM_O_NONBLOCK;
        if (k_do_pipe_flags(fds, pipe_flags) < 0) { args->ret = (unsigned long)-ENOMEM; return; }
        audio_cap_file = k_close_fd_get_file(fds[0]);
        logki("vdrm: audio cap pipe created\n");
        args->ret = (unsigned long)fds[1];
        return;
    }

    if (strcmp(cmd, "audio/play_recv") == 0) {
        if (!audio_play_file) { args->ret = (unsigned long)-EAGAIN; return; }
        int fd = k_get_unused_fd_flags(0);
        if (fd < 0) { args->ret = (unsigned long)-ENOMEM; return; }
        k_fd_install(fd, audio_play_file);
        audio_play_file = NULL;
        logki("vdrm: audio play_recv transferred\n");
        args->ret = (unsigned long)fd;
        return;
    }

    if (strcmp(cmd, "audio/cap_recv") == 0) {
        if (!audio_cap_file) { args->ret = (unsigned long)-EAGAIN; return; }
        int fd = k_get_unused_fd_flags(0);
        if (fd < 0) { args->ret = (unsigned long)-ENOMEM; return; }
        k_fd_install(fd, audio_cap_file);
        audio_cap_file = NULL;
        logki("vdrm: audio cap_recv transferred\n");
        args->ret = (unsigned long)fd;
        return;
    }

    args->ret = (unsigned long)-EINVAL;
}

/* ---- Init / Exit / Ctl ---- */
static long vdrm_init(const char *args, const char *event, void *__user reserved)
{
    void *addr;
    hook_err_t err;

    vdrm_enabled = 0;
    container_root_dentry = NULL;
    apk_buf_count = 0;
    apk_pending_idx = -1;
    apk_flip_done = 0;
    apk_present_pid = 0;
    v_drm_dev = NULL;
    v_drm_dev_captured = 0;
    ev_ring.head = 0; ev_ring.tail = 0;
    audio_play_file = NULL;
    audio_cap_file = NULL;

    compat_copy_from_user = (void *)kallsyms_lookup_name("__arch_copy_from_user");
    if (!compat_copy_from_user)
        compat_copy_from_user = (void *)kallsyms_lookup_name("copy_from_user");
    if (!compat_copy_from_user)
        compat_copy_from_user = (void *)kallsyms_lookup_name("_copy_from_user");
    if (!compat_copy_from_user) { logke("vdrm: copy_from_user not found\n"); return -EFAULT; }

    k_copy_to_user = (void *)kallsyms_lookup_name("_copy_to_user");
    if (!k_copy_to_user)
        k_copy_to_user = (void *)kallsyms_lookup_name("copy_to_user");
    if (!k_copy_to_user)
        k_copy_to_user = (void *)kallsyms_lookup_name("__arch_copy_to_user");
    if (!k_copy_to_user) { logke("vdrm: copy_to_user not found\n"); return -EFAULT; }

    /* ---- Container detection hook ---- */
    addr = (void *)kallsyms_lookup_name("__arm64_sys_chroot");
    if (!addr) addr = (void *)kallsyms_lookup_name("sys_chroot");
    if (addr) {
        err = hook_wrap1(addr, NULL, after_chroot, NULL);
        if (err == HOOK_NO_ERR) orig_sys_chroot = addr;
        else logke("vdrm: hook chroot failed: %d\n", err);
    } else logkw("vdrm: sys_chroot not found\n");

    /* ---- DRM function lookups ---- */
    k_drm_gem_object_lookup = (void *)kallsyms_lookup_name("drm_gem_object_lookup");
    if (!k_drm_gem_object_lookup) { logke("vdrm: drm_gem_object_lookup not found\n"); return -EFAULT; }

    k_dma_buf_get = (void *)kallsyms_lookup_name("dma_buf_get");
    if (!k_dma_buf_get) { logke("vdrm: dma_buf_get not found\n"); return -EFAULT; }

    k_dma_buf_fd = (void *)kallsyms_lookup_name("dma_buf_fd");
    if (!k_dma_buf_fd) { logke("vdrm: dma_buf_fd not found\n"); return -EFAULT; }

    k_dma_buf_put = (void *)kallsyms_lookup_name("dma_buf_put");
    if (!k_dma_buf_put) { logke("vdrm: dma_buf_put not found\n"); return -EFAULT; }

    k_strncpy_from_user = (void *)kallsyms_lookup_name("strncpy_from_user");
    if (!k_strncpy_from_user) { logke("vdrm: strncpy_from_user not found\n"); return -EFAULT; }

    k_msleep_interruptible = (void *)kallsyms_lookup_name("msleep_interruptible");
    if (!k_msleep_interruptible) { logke("vdrm: msleep_interruptible not found\n"); return -EFAULT; }

    k_wake_up_process = (void *)kallsyms_lookup_name("wake_up_process");
    if (!k_wake_up_process) { logke("vdrm: wake_up_process not found\n"); return -EFAULT; }

    k_find_task_by_vpid = (void *)kallsyms_lookup_name("find_task_by_vpid");
    if (!k_find_task_by_vpid) { logke("vdrm: find_task_by_vpid not found\n"); return -EFAULT; }

    k_task_pid_nr = (void *)kallsyms_lookup_name("__task_pid_nr_ns");
    if (!k_task_pid_nr) { logke("vdrm: __task_pid_nr_ns not found\n"); return -EFAULT; }

    k_task_active_pid_ns = (void *)kallsyms_lookup_name("task_active_pid_ns");
    if (!k_task_active_pid_ns) { logkw("vdrm: task_active_pid_ns not found, will use NULL ns\n"); }

    k_rcu_read_lock = (void *)kallsyms_lookup_name("__rcu_read_lock");
    k_rcu_read_unlock = (void *)kallsyms_lookup_name("__rcu_read_unlock");
    if (!k_rcu_read_lock || !k_rcu_read_unlock) { logke("vdrm: __rcu_read_lock/unlock not found\n"); return -EFAULT; }
    logki("vdrm: rcu_read_lock=%p rcu_read_unlock=%p\n", k_rcu_read_lock, k_rcu_read_unlock);

    k_do_pipe_flags = (void *)kallsyms_lookup_name("do_pipe_flags");
    if (!k_do_pipe_flags) { logke("vdrm: do_pipe_flags not found\n"); return -EFAULT; }

    k_close_fd_get_file = (void *)kallsyms_lookup_name("close_fd_get_file");
    if (!k_close_fd_get_file) { logke("vdrm: close_fd_get_file not found\n"); return -EFAULT; }

    k_get_unused_fd_flags = (void *)kallsyms_lookup_name("get_unused_fd_flags");
    if (!k_get_unused_fd_flags) { logke("vdrm: get_unused_fd_flags not found\n"); return -EFAULT; }

    k_fd_install = (void *)kallsyms_lookup_name("fd_install");
    if (!k_fd_install) { logke("vdrm: fd_install not found\n"); return -EFAULT; }

    k_fput = (void *)kallsyms_lookup_name("fput");
    if (!k_fput) { logke("vdrm: fput not found\n"); return -EFAULT; }

    /* ---- DRM ioctl hook ---- */
    addr = (void *)kallsyms_lookup_name("drm_ioctl");
    if (!addr) { logke("vdrm: drm_ioctl not found\n"); return -EFAULT; }
    err = hook_wrap3(addr, before_ioctl, NULL, NULL);
    if (err != HOOK_NO_ERR) { logke("vdrm: hook drm_ioctl failed: %d\n", err); return -EFAULT; }
    orig_drm_ioctl = addr;

    /* ---- openat hook ---- */
    addr = (void *)kallsyms_lookup_name("__arm64_sys_openat");
    if (!addr) { logkw("vdrm: __arm64_sys_openat not found\n"); }
    else {
        err = hook_wrap1(addr, before_openat, NULL, NULL);
        if (err != HOOK_NO_ERR) logke("vdrm: hook openat failed: %d\n", err);
        else { orig_sys_openat = addr; logki("vdrm: openat hook installed\n"); }
    }

    logki("vdrm: v0.4.0 init done\n");
    return 0;
}

static long vdrm_exit(void *__user reserved)
{
    if (orig_drm_ioctl) hook_unwrap(orig_drm_ioctl, before_ioctl, NULL);
    if (orig_sys_chroot) hook_unwrap(orig_sys_chroot, NULL, after_chroot);
    if (orig_sys_openat) hook_unwrap(orig_sys_openat, before_openat, NULL);

    /* Wake APK if blocked */
    struct task_struct *wtask = NULL;
    vdrm_spin_lock(&vdrm_lock);
    logki("vdrm: exit present_pid=%d\n", apk_present_pid);
    if (k_rcu_read_lock) k_rcu_read_lock();
    if (apk_present_pid) {
        wtask = k_find_task_by_vpid(apk_present_pid);
        logki("vdrm: exit find_task_by_vpid(%d)=%p\n", apk_present_pid, wtask);
        apk_present_pid = 0;
    }
    vdrm_spin_unlock(&vdrm_lock);
    if (wtask) {
        apk_flip_done = 1;
        k_wake_up_process(wtask);
        logki("vdrm: exit wake_up_process done\n");
    }
    if (k_rcu_read_unlock) k_rcu_read_unlock();

    if (audio_play_file) { k_fput(audio_play_file); audio_play_file = NULL; }
    if (audio_cap_file)  { k_fput(audio_cap_file);  audio_cap_file  = NULL; }

    for (int __i = 0; __i < apk_buf_count; __i++) {
        if (apk_bufs[__i]) {
            logki("vdrm: exit dma_buf_put [%d]=%p\n", __i, apk_bufs[__i]);
            k_dma_buf_put(apk_bufs[__i]);
        }
    }
    apk_buf_count = 0;

    container_root_dentry = NULL;
    return 0;
}

static long vdrm_ctl0(const char *ctl_args, char *__user out_msg, int outlen)
{
    if (!ctl_args) return 0;
    if (strcmp(ctl_args, "enable") == 0) vdrm_enabled = 1;
    else if (strcmp(ctl_args, "disable") == 0) vdrm_enabled = 0;
    else if (strcmp(ctl_args, "reset") == 0) {
        vdrm_spin_lock(&vdrm_lock);
        fb_count = 0;
        memset(fb_table, 0, sizeof(fb_table));
        for (int __i = 0; __i < apk_buf_count; __i++) {
            if (apk_bufs[__i]) {
                logki("vdrm: reset dma_buf_put [%d]=%p\n", __i, apk_bufs[__i]);
                k_dma_buf_put(apk_bufs[__i]);
            }
        }
        apk_buf_count = 0;
        memset(apk_bufs, 0, sizeof(apk_bufs));
        vdrm_spin_unlock(&vdrm_lock);
        logki("vdrm: reset ok\n");
    } else if (strcmp(ctl_args, "status") == 0 && out_msg && outlen > 0) {
        char buf[64]; int p = 0;
        buf[p++] = 'e'; buf[p++] = 'n'; buf[p++] = '=';
        buf[p++] = vdrm_enabled ? '1' : '0';
        buf[p++] = ' '; buf[p++] = 'b'; buf[p++] = '=';
        { int v = apk_buf_count; if (v == 0) buf[p++] = '0'; else { char t[8]; int ti = 0; while (v) { t[ti++] = '0' + (v % 10); v /= 10; } while (ti) buf[p++] = t[--ti]; } }
        buf[p++] = ' '; buf[p++] = 'f'; buf[p++] = '=';
        { int v = fb_count; if (v == 0) buf[p++] = '0'; else { char t[8]; int ti = 0; while (v) { t[ti++] = '0' + (v % 10); v /= 10; } while (ti) buf[p++] = t[--ti]; } }
        buf[p++] = ' '; buf[p++] = 'c'; buf[p++] = '=';
        buf[p++] = container_root_dentry ? '1' : '0';
        buf[p++] = '\n';
        if (p < outlen) { buf[p] = 0; k_copy_to_user(out_msg, buf, p); }
        return 0;
    }

    if (strcmp(ctl_args, "bufs") == 0) {
        vdrm_spin_lock(&vdrm_lock);
        logki("vdrm: bufs: count=%d\n", apk_buf_count);
        for (int i = 0; i < apk_buf_count; i++)
            logki("vdrm:   [%d] dmabuf=%p\n", i, apk_bufs[i]);
        vdrm_spin_unlock(&vdrm_lock);
        return 0;
    }

    return 0;
}

KPM_INIT(vdrm_init);
KPM_CTL0(vdrm_ctl0);
KPM_EXIT(vdrm_exit);