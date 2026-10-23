/* inventory.c: parse each declared case, mapping and prepared root once. */
#define _POSIX_C_SOURCE 200809L
#include "collection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
static int case_line(struct nc_inventory *i, const char *text)
{
    struct nc_case *c;
    char *word, *save = NULL;
    size_t n;
    int command = 0;
    if (i->ncases == NC_CASE_MAX)
        return 0;
    c = &i->cases[i->ncases];
    c->line = strdup(text);
    if (!c->line)
        return 0;
    word = strtok_r(c->line, " \t\r\n", &save);
    if (!word || strcmp(word, "case"))
        return 0;
    c->name = strtok_r(NULL, " \t\r\n", &save);
    if (!c->name || !*c->name || strstr(c->name, "..") || c->name[0] == '/')
        return 0;
    for (n = 0; n < i->ncases; n++)
        if (!strcmp(i->cases[n].name, c->name))
            return 0;
    while ((word = strtok_r(NULL, " \t\r\n", &save))) {
        if (!command && !strcmp(word, "--")) {
            command = 1;
            continue;
        }
        if (command) {
            if (c->argc + 1 == NC_WORD_MAX)
                return 0;
            c->argv[c->argc++] = word;
        } else {
            if (c->nfixtures == NC_WORD_MAX)
                return 0;
            c->fixtures[c->nfixtures++] = word;
        }
    }
    if (!command || !c->argc)
        return 0;
    i->ncases++;
    return 1;
}
static int cases_file(struct nc_inventory *i, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[8192], joined[32768] = "";
    size_t used = 0;
    unsigned number = 0;
    int ok = fp != NULL;
    while (fp && fgets(line, sizeof(line), fp)) {
        char *comment = strchr(line, '#'), *at;
        size_t n;
        number++;
        if (comment)
            *comment = 0;
        n = strlen(line);
        while (n && isspace((unsigned char)line[n - 1]))
            line[--n] = 0;
        if (used + n + 2 >= sizeof(joined)) {
            ok = 0;
            break;
        }
        memcpy(joined + used, line, n + 1);
        used += n;
        if (used && joined[used - 1] == '\\') {
            joined[used - 1] = ' ';
            continue;
        }
        at = joined + strspn(joined, " \t");
        if (*at && !case_line(i, at)) {
            ok = 0;
            break;
        }
        used = 0;
        joined[0] = 0;
    }
    if (fp) {
        ok &= !ferror(fp) && !used;
        fclose(fp);
    }
    if (!ok)
        fprintf(stderr, "native: %s:%u: invalid or incomplete inventory\n", path, number);
    return ok;
}
int nc_map_value(const struct nc_inventory *i, const char *kind, unsigned old, unsigned *value)
{
    size_t n;
    for (n = 0; n < i->nmaps; n++)
        if (!strcmp(i->maps[n].kind, kind) && i->maps[n].old == old) {
            *value = i->maps[n].native;
            return 1;
        }
    return 0;
}
int nc_image_has(const struct nc_inventory *i, const char *name, const char *type)
{
    for (size_t n = 0; n < i->nimages; n++)
        if (!strcmp(i->images[n].name, name) && (!type || !strcmp(i->images[n].type, type)))
            return 1;
    return 0;
}
static int image_line(struct nc_inventory *i, const char *line)
{
    struct nc_image *image;
    if (i->nimages == 128)
        return 0;
    image = &i->images[i->nimages];
    if (sscanf(line, "%127s %15s %u", image->name, image->type, &image->mib) != 3 || !image->mib ||
        strchr(image->name, '/') || strstr(image->name, "..") || nc_image_has(i, image->name, NULL))
        return 0;
    if (strcmp(image->type, "krffs") && strcmp(image->type, "ext2") &&
        strcmp(image->type, "vfat") && strcmp(image->type, "exfat"))
        return 0;
    i->nimages++;
    return 1;
}
static int map_line(struct nc_inventory *i, char *line)
{
    char *save = NULL, *kind = strtok_r(line, " \t\r\n", &save), *word;
    if (!kind || *kind == '#')
        return 1;
    if (!strcmp(kind, "account"))
        return sscanf(save, "%79s %u", i->account, &i->uid) == 2;
    if (strcmp(kind, "idmap") && strcmp(kind, "portmap"))
        return !strcmp(kind, "image") && image_line(i, save);
    while ((word = strtok_r(NULL, " \t\r\n", &save))) {
        struct nc_map *m;
        unsigned ignored;
        char tail;
        if (*word == '#')
            break;
        if (i->nmaps == 256)
            return 0;
        m = &i->maps[i->nmaps];
        if (sscanf(word, "%u:%u%c", &m->old, &m->native, &tail) != 2 ||
            nc_map_value(i, kind, m->old, &ignored))
            return 0;
        for (size_t n = 0; n < i->nmaps; n++)
            if (!strcmp(i->maps[n].kind, kind) && i->maps[n].native == m->native)
                return 0;
        snprintf(m->kind, sizeof(m->kind), "%s", kind);
        i->nmaps++;
    }
    return 1;
}
static int auxiliary(struct nc_inventory *i, const struct nc_config *conf, int roots)
{
    char path[4096], line[8192];
    FILE *fp;
    int ok = 1;
    snprintf(path, sizeof(path), "%s/%s", roots ? conf->images : conf->inventory,
             roots ? "roots" : "maps");
    fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *at = line + strspn(line, " \t\r\n");
        if (!*at || *at == '#')
            continue;
        if (roots) {
            struct nc_root *r;
            char tail;
            if (i->nroots == NC_CASE_MAX) {
                ok = 0;
                break;
            }
            r = &i->roots[i->nroots];
            if (sscanf(at, "%127s %127s %c", r->name, r->image, &tail) != 2 ||
                strchr(r->image, '/') || strstr(r->image, "..")) {
                ok = 0;
                break;
            }
            for (size_t n = 0; n < i->nroots; n++)
                if (!strcmp(i->roots[n].name, r->name))
                    ok = 0;
            i->nroots++;
        } else
            ok &= map_line(i, at);
    }
    ok &= !ferror(fp);
    fclose(fp);
    return ok;
}
int nc_inventory_read(struct nc_inventory *out, const struct nc_config *conf, int with_roots)
{
    static const char *files[] = {"probes.cases", "musl.cases", "rows.cases"};
    char path[4096];
    unsigned n;
    memset(out, 0, sizeof(*out));
    for (n = 0; n < 3; n++) {
        snprintf(path, sizeof(path), "%s/%s", conf->inventory, files[n]);
        if (!cases_file(out, path))
            return 0;
    }
    return out->ncases && auxiliary(out, conf, 0) && (!with_roots || auxiliary(out, conf, 1));
}
void nc_inventory_free(struct nc_inventory *i)
{
    size_t n;
    for (n = 0; n < NC_CASE_MAX; n++)
        free(i->cases[n].line);
    memset(i, 0, sizeof(*i));
}
