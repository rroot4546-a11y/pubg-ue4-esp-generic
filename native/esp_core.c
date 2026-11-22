#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

typedef uint64_t u64;

typedef struct { u64 s, e; int x, w; char path[512]; } Map;
typedef struct {
    u64 GWorld, Level, Actors, Root, C2W, FirstPC, AckPawn, CamMgr, CamPOV;
} Offsets;
typedef struct { float x, y, z; } Vec;
typedef struct { float w, x, y, z; } Quat;
typedef struct { Quat q; Vec t; } Xform;
typedef struct { Vec fwd, right, up, loc; float fov; } Cam;

#define MAX_MAPS 2048
#define MAX_ACTORS 4096
#define SCAN_CHUNK (1u << 16)

static Map g_maps[MAX_MAPS];
static int g_nmaps;
static pid_t g_pid;
static u64 g_modlo, g_modhi, g_base;
static char g_lib[128] = "libUE4.so";
static int g_sw = 1080, g_sh = 2400;
static Offsets g_off;

static const char *const LIBS[] = {
    "libUE4.so", "libue4.so", "libUE4.so", "libGame.so",
    "libUnreal.so", "libpubg.so", "libTencent.so"
};
static const u64 PERSIST_OFFS[] = {
    0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58,0x60,0x68,0x70,0x78,
    0x80,0x88,0x90,0x98,0xa0,0xa8,0xb0,0xc0,0xd0,0xe0,0xf0,0x100,
    0x110,0x120,0x140,0x160,0x180,0x1a0,0x1c0,0x200
};
static const u64 ACTOR_OFFS[] = {
    0x70,0x78,0x80,0x88,0x90,0x98,0xa0,0xa8,0xb0,0xb8,0xc0,0xc8,
    0xd0,0xd8,0xe0,0xe8,0xf0,0xf8,0x100,0x110,0x120,0x140,0x160
};

static int rd(u64 a, void *b, size_t n) {
    struct iovec lo = { b, n }, ro = { (void *)a, n };
    return syscall(SYS_process_vm_readv, g_pid, &lo, 1, &ro, 1, 0) == (ssize_t)n ? 0 : -1;
}
static u64 r64(u64 a) { u64 v = 0; rd(a, &v, sizeof v); return v; }
static int32_t r32(u64 a) { int32_t v = 0; rd(a, &v, sizeof v); return v; }
static float rfl(u64 a) { float v = 0; rd(a, &v, sizeof v); return v; }

static void trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int load_cfg(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "lib")) {
            snprintf(g_lib, sizeof g_lib, "%s", v);
            continue;
        }
        if (strcmp(k, "width") == 0) { g_sw = atoi(v); continue; }
        if (strcmp(k, "height") == 0) { g_sh = atoi(v); continue; }
        u64 n = (u64)strtoull(v, NULL, 0);
        if      (!strcmp(k, "GWorld"))             g_off.GWorld = n;
        else if (!strcmp(k, "UWorld_Level"))       g_off.Level = n;
        else if (!strcmp(k, "ULevel_Actors"))      g_off.Actors = n;
        else if (!strcmp(k, "AActor_Root"))        g_off.Root = n;
        else if (!strcmp(k, "UScene_C2W"))         g_off.C2W = n;
        else if (!strcmp(k, "UWorld_FirstPC"))     g_off.FirstPC = n;
        else if (!strcmp(k, "PC_AckPawn"))         g_off.AckPawn = n;
        else if (!strcmp(k, "PC_CamMgr"))          g_off.CamMgr = n;
        else if (!strcmp(k, "CamMgr_POV"))         g_off.CamPOV = n;
    }
    fclose(f);
    return 1;
}

static pid_t next_pid(const char *pkg, pid_t after) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    pid_t found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        pid_t p = (pid_t)atoi(e->d_name);
        if (p <= after || p <= 1) continue;
        char path[96], buf[8192];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        for (ssize_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
        if (strstr(buf, pkg)) { found = p; break; }
    }
    closedir(d);
    return found;
}

static void load_maps(void) {
    g_nmaps = 0;
    char path[80];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)g_pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f) && g_nmaps < MAX_MAPS) {
        u64 s = 0, e = 0;
        char prot[8] = {0};
        if (sscanf(line, "%llx-%llx %4s",
                   (unsigned long long *)&s, (unsigned long long *)&e, prot) != 3) continue;
        Map *m = &g_maps[g_nmaps++];
        m->s = s; m->e = e;
        m->x = strchr(prot, 'x') != NULL;
        m->w = strchr(prot, 'w') != NULL;
        const char *slash = strchr(line, '/');
        if (slash) {
            snprintf(m->path, sizeof m->path, "%s", slash);
            char *nl = strchr(m->path, '\n');
            if (nl) *nl = 0;
        }
    }
    fclose(f);
}

static int inmap(u64 a) {
    for (int i = 0; i < g_nmaps; i++)
        if (a >= g_maps[i].s && a < g_maps[i].e) return 1;
    return 0;
}
static int inexec(u64 a) {
    for (int i = 0; i < g_nmaps; i++)
        if (g_maps[i].x && a >= g_maps[i].s && a < g_maps[i].e) return 1;
    return 0;
}
static int is_obj(u64 p) { return inmap(p) && inexec(r64(p)); }

static int detect_module(void) {
    for (unsigned k = 0; k < sizeof LIBS / sizeof LIBS[0]; k++) {
        for (int i = 0; i < g_nmaps; i++) {
            if (strstr(g_maps[i].path, LIBS[k])) {
                snprintf(g_lib, sizeof g_lib, "%s", LIBS[k]);
                return 1;
            }
        }
    }
    return 0;
}

static int module_range(void) {
    for (int i = 0; i < g_nmaps; i++) {
        if (!strstr(g_maps[i].path, g_lib)) continue;
        if (!g_modlo || g_maps[i].s < g_modlo) g_modlo = g_maps[i].s;
        if (g_maps[i].e > g_modhi) g_modhi = g_maps[i].e;
    }
    if (!g_modlo || !g_modhi) return 0;
    for (int i = 0; i < g_nmaps; i++) {
        if (g_maps[i].x && g_maps[i].s >= g_modlo && g_maps[i].s < g_modhi) {
            g_base = g_maps[i].s;
            break;
        }
    }
    return g_base != 0;
}

static int try_target(pid_t candidate) {
    g_pid = candidate;
    g_modlo = 0;
    g_modhi = 0;
    g_base = 0;
    load_maps();
    if (!detect_module()) return 0;
    return module_range();
}

static int plausible_actors(u64 level, u64 *data_out, int *count_out, u64 *off_out) {
    for (unsigned k = 0; k < sizeof ACTOR_OFFS / sizeof ACTOR_OFFS[0]; k++) {
        u64 o = ACTOR_OFFS[k];
        u64 d = r64(level + o);
        if (!inmap(d)) continue;
        int n = r32(level + o + 8);
        if (n < 16 || n > MAX_ACTORS) continue;
        u64 a0 = r64(d);
        if (is_obj(a0)) {
            *data_out = d; *count_out = n; *off_out = o;
            return 1;
        }
    }
    return 0;
}

static int discover_world(void) {
    unsigned char *buf = malloc(SCAN_CHUNK);
    if (!buf) return 0;
    int ok = 0;
    for (int i = 0; i < g_nmaps && !ok; i++) {
        Map *m = &g_maps[i];
        if (!m->w) continue;
        u64 start = m->s, end = m->e;
        if (start < g_modlo) start = g_modlo;
        if (end > g_modhi) end = g_modhi;
        if (end <= start) continue;
        for (u64 a = start; a + SCAN_CHUNK <= end && !ok; a += SCAN_CHUNK) {
            if (rd(a, buf, SCAN_CHUNK)) continue;
            for (u64 o = 0; o + 8 <= SCAN_CHUNK && !ok; o += 8) {
                u64 p;
                memcpy(&p, buf + o, 8);
                if (!is_obj(p)) continue;
                for (unsigned k = 0; k < sizeof PERSIST_OFFS / sizeof PERSIST_OFFS[0] && !ok; k++) {
                    u64 lv = r64(p + PERSIST_OFFS[k]);
                    if (!is_obj(lv)) continue;
                    u64 data; int count; u64 aoff;
                    if (!plausible_actors(lv, &data, &count, &aoff)) continue;
                    g_off.GWorld = a + o - g_base;
                    g_off.Level = PERSIST_OFFS[k];
                    g_off.Actors = aoff;
                    ok = 1;
                }
            }
        }
    }
    free(buf);
    return ok;
}

static int actor_index(u64 data, int count, u64 ptr) {
    if (!ptr) return 0;
    int lim = count < 160 ? count : 160;
    for (int i = 0; i < lim; i++) if (r64(data + (u64)i * 8) == ptr) return 1;
    return 0;
}

static int pov_ok(u64 a) {
    float x = rfl(a), y = rfl(a + 4), z = rfl(a + 8);
    float p = rfl(a + 0xC), ya = rfl(a + 0x10), rr = rfl(a + 0x14);
    float f = rfl(a + 0x18);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    if (fabsf(x) > 1e6f || fabsf(y) > 1e6f || fabsf(z) > 1e6f) return 0;
    if (!isfinite(p) || !isfinite(ya) || !isfinite(rr) || !isfinite(f)) return 0;
    if (fabsf(p) > 180.f || fabsf(ya) > 360.f || fabsf(rr) > 180.f) return 0;
    if (f < 40.f || f > 150.f) return 0;
    if (fabsf(p) + fabsf(ya) + fabsf(rr) < 0.001f) return 0;
    return 1;
}

static int discover_controller(u64 world, u64 data, int count) {
    for (u64 off = 0; off <= 0x500; off += 8) {
        u64 pc = r64(world + off);
        if (!actor_index(data, count, pc)) continue;
        u64 ack = 0, cam = 0, pov = 0;
        for (u64 o = 0; o <= 0x500 && !(ack && cam); o += 8) {
            u64 v = r64(pc + o);
            if (!v || v == pc) continue;
            if (!ack && actor_index(data, count, v)) ack = o;
            if (!cam && is_obj(v)) {
                for (u64 q = 0; q <= 0x500; q += 4) {
                    if (pov_ok(v + q)) { cam = o; pov = q; break; }
                }
            }
        }
        if (ack && cam) {
            g_off.FirstPC = off; g_off.AckPawn = ack;
            g_off.CamMgr = cam; g_off.CamPOV = pov;
            return 1;
        }
    }
    return 0;
}

static int transform_ok(u64 a) {
    float w = rfl(a), x = rfl(a + 4), y = rfl(a + 8), z = rfl(a + 12);
    if (!isfinite(w) || !isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    float n = w * w + x * x + y * y + z * z;
    if (n < 0.7f || n > 1.3f) return 0;
    float tx = rfl(a + 0x10), ty = rfl(a + 0x14), tz = rfl(a + 0x18);
    if (!isfinite(tx) || !isfinite(ty) || !isfinite(tz)) return 0;
    if (fabsf(tx) > 1e6f || fabsf(ty) > 1e6f || fabsf(tz) > 1e6f) return 0;
    return 1;
}

static int discover_root(u64 data, int count) {
    int n = count < 24 ? count : 24;
    u64 s[24];
    for (int i = 0; i < n; i++) s[i] = r64(data + (u64)i * 8);
    int best_r = -1, best_c = -1, best = 0;
    for (u64 ro = 0x28; ro <= 0x500; ro += 8) {
        u64 roots[24];
        int valid = 0;
        for (int i = 0; i < n; i++) {
            roots[i] = r64(s[i] + ro);
            if (is_obj(roots[i])) valid++;
        }
        if (valid < 2) continue;
        for (u64 co = 0; co <= 0x300; co += 0x10) {
            int score = 0;
            for (int i = 0; i < n; i++)
                if (is_obj(roots[i]) && transform_ok(roots[i] + co)) score++;
            if (score > best) { best = score; best_r = (int)ro; best_c = (int)co; }
        }
    }
    if (best < 2 || best_r < 0 || best_c < 0) return 0;
    g_off.Root = (u64)best_r;
    g_off.C2W = (u64)best_c;
    return 1;
}

static Vec vec(u64 a) { Vec v = { rfl(a), rfl(a + 4), rfl(a + 8) }; return v; }
static Vec vsub(Vec a, Vec b) { Vec v = { a.x-b.x, a.y-b.y, a.z-b.z }; return v; }
static float vdot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec vcross(Vec a, Vec b) {
    Vec v = { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
    return v;
}
static float vlen(Vec a) { return sqrtf(vdot(a, a)); }
static Vec qrot(Quat q, Vec v) {
    float tx = 2.f * (q.y * v.z - q.z * v.y);
    float ty = 2.f * (q.z * v.x - q.x * v.z);
    float tz = 2.f * (q.x * v.y - q.y * v.x);
    Vec r = {
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx)
    };
    return r;
}
static Vec xform(Xform t, Vec p) {
    Vec r = qrot(t.q, p);
    Vec o = { r.x + t.t.x, r.y + t.t.y, r.z + t.t.z };
    return o;
}

static Cam read_cam(u64 pov) {
    Cam c;
    c.loc = vec(pov);
    float p = rfl(pov + 0xC) * 0.01745329252f;
    float y = rfl(pov + 0x10) * 0.01745329252f;
    float rr = rfl(pov + 0x14) * 0.01745329252f;
    float cp = cosf(p), sp = sinf(p);
    float cy = cosf(y), sy = sinf(y);
    float cr = cosf(rr), sr = sinf(rr);
    c.fwd = (Vec){ cp*cy, cp*sy, sp };
    c.right = (Vec){ sr*sp*cy - cr*sy, sr*sp*sy + cr*cy, -sr*cp };
    c.up = vcross(c.fwd, c.right);
    c.fov = rfl(pov + 0x18);
    if (c.fov < 1.f || c.fov > 179.f) c.fov = 90.f;
    return c;
}

static int project(Vec p, const Cam *c, float *ox, float *oy, float *oz) {
    Vec d = vsub(p, c->loc);
    float z = vdot(d, c->fwd);
    if (z <= 1.f || z > 20000.f) return 0;
    float x = vdot(d, c->right), yy = vdot(d, c->up);
    float tan = tanf(c->fov * 0.5f * 0.01745329252f);
    if (tan <= 0.f) return 0;
    float nx = x / (z * tan);
    float ny = (yy * (float)g_sh / (float)g_sw) / (z * tan);
    *ox = (nx + 1.f) * 0.5f * (float)g_sw;
    *oy = (1.f - ny) * 0.5f * (float)g_sh;
    *oz = z;
    return 1;
}

static void emit_frame(void) {
    u64 world = r64(g_base + g_off.GWorld);
    if (!world) return;
    u64 level = r64(world + g_off.Level);
    if (!level) return;
    u64 data = r64(level + g_off.Actors);
    if (!data) return;
    int num = r32(level + g_off.Actors + 8);
    if (num < 0 || num > MAX_ACTORS) return;

    u64 pc = r64(world + g_off.FirstPC);
    if (!pc) return;
    u64 mgr = r64(pc + g_off.CamMgr);
    if (!mgr) return;
    Cam cam = read_cam(mgr + g_off.CamPOV);
    u64 local = r64(pc + g_off.AckPawn);
    float focal = (float)g_sh / (2.f * tanf(cam.fov * 0.5f * 0.01745329252f));

    for (int i = 0; i < num; i++) {
        u64 a = r64(data + (u64)i * 8);
        if (!a || a == local) continue;
        u64 rootp = r64(a + g_off.Root);
        if (!rootp) continue;
        float wq = rfl(rootp + g_off.C2W);
        float xq = rfl(rootp + g_off.C2W + 4);
        float yq = rfl(rootp + g_off.C2W + 8);
        float zq = rfl(rootp + g_off.C2W + 12);
        if (!isfinite(wq) || !isfinite(xq) || !isfinite(yq) || !isfinite(zq)) continue;
        Quat q = { wq, xq, yq, zq };
        Xform t;
        t.q = q;
        t.t = vec(rootp + g_off.C2W + 0x10);
        Vec center = xform(t, (Vec){0.f, 0.f, 0.f});
        float dist = vlen(vsub(center, cam.loc));
        if (dist > 500.f || dist < 0.5f) continue;

        float ox, oy, oz;
        if (!project(center, &cam, &ox, &oy, &oz)) continue;
        float h = 175.f * focal / oz;
        float wdt = h * 0.42f;
        if (h < 8.f || h > (float)g_sh * 3.f) continue;
        float l = ox - wdt * 0.5f, rr = ox + wdt * 0.5f;
        float tp = oy - h, bt = oy;
        if (rr < 0.f || l > (float)g_sw || bt < 0.f || tp > (float)g_sh) continue;
        if (l < 0.f) l = 0.f;
        if (tp < 0.f) tp = 0.f;
        if (rr > (float)g_sw) rr = (float)g_sw;
        if (bt > (float)g_sh) bt = (float)g_sh;
        printf("T %.1f %.1f %.1f %.1f 1 %d %.1f %.1f 0\n",
               (double)l, (double)tp, (double)rr, (double)bt, (int)dist,
               (double)ox, (double)oy);
    }
    printf("F\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <package> <width> <height> [config]\n", argv[0]);
        return 2;
    }
    const char *pkg = argv[1];
    g_sw = atoi(argv[2]);
    g_sh = atoi(argv[3]);
    if (g_sw <= 0 || g_sh <= 0) return 2;
    if (argc > 4) load_cfg(argv[4]);

    fprintf(stderr, "# waiting for %s\n", pkg);
    int attached = 0;
    for (int wait = 0; wait < 1200 && !attached; wait++) {
        pid_t candidate = 0;
        while ((candidate = next_pid(pkg, candidate))) {
            if (try_target(candidate)) { attached = 1; break; }
        }
        if (!attached) usleep(250000);
    }
    if (!attached) {
        fprintf(stderr, "# target module not mapped yet\n");
        return 4;
    }
    fprintf(stderr, "# pid=%d lib=%s base=0x%llx\n", (int)g_pid, g_lib,
            (unsigned long long)g_base);

    if (!g_off.GWorld) {
        if (!discover_world()) { fprintf(stderr, "# GWorld discovery failed\n"); return 6; }
        fprintf(stderr, "# discovered GWorld=0x%llx Level=0x%llx Actors=0x%llx\n",
                (unsigned long long)g_off.GWorld, (unsigned long long)g_off.Level,
                (unsigned long long)g_off.Actors);
    }
    u64 world = r64(g_base + g_off.GWorld);
    if (!world) { fprintf(stderr, "# empty GWorld slot\n"); return 7; }
    u64 level = r64(world + g_off.Level);
    if (!is_obj(level)) { fprintf(stderr, "# bad level offset\n"); return 8; }
    u64 data; int count; u64 aoff;
    if (!plausible_actors(level, &data, &count, &aoff)) {
        fprintf(stderr, "# bad actor array\n");
        return 9;
    }
    if (!g_off.Actors) g_off.Actors = aoff;
    discover_controller(world, data, count);
    discover_root(data, count);
    fprintf(stderr, "# discovered PC=0x%llx Ack=0x%llx Cam=0x%llx POV=0x%llx Root=0x%llx C2W=0x%llx\n",
            (unsigned long long)g_off.FirstPC, (unsigned long long)g_off.AckPawn,
            (unsigned long long)g_off.CamMgr, (unsigned long long)g_off.CamPOV,
            (unsigned long long)g_off.Root, (unsigned long long)g_off.C2W);
    if (!g_off.FirstPC || !g_off.Root) {
        fprintf(stderr, "# partial discovery, boxes may be missing\n");
    }

    fprintf(stderr, "# streaming\n");
    for (;;) {
        emit_frame();
        usleep(16000);
    }
}
