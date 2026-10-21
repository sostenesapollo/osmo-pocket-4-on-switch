/*
 * mtp_install — limpa versões antigas do Osmo View e instala a nova.
 *
 *   mtp_install clean                 lista e apaga arquivos/pastas osmo*
 *   mtp_install push <nro> <host> <name>
 *
 * O DBI recusa gravar em subpasta criada por MTP, então instalamos direto
 * em sdmc:/switch/.
 */
#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SWITCH_PARENT 0x01000003u /* sdmc:/switch */

typedef struct {
  uint32_t id;
  uint32_t parent;
  uint32_t storage;
  int is_folder;
  char name[256];
} Entry;

static Entry g_entries[8192];
static int g_count = 0;

static void load_entries(LIBMTP_mtpdevice_t *dev) {
  g_count = 0;
  LIBMTP_file_t *files = LIBMTP_Get_Filelisting_With_Callback(dev, NULL, NULL);
  for (LIBMTP_file_t *f = files; f && g_count < 8192; f = f->next) {
    Entry *e = &g_entries[g_count++];
    e->id = f->item_id;
    e->parent = f->parent_id;
    e->storage = f->storage_id;
    e->is_folder = (f->filetype == LIBMTP_FILETYPE_FOLDER);
    snprintf(e->name, sizeof(e->name), "%s", f->filename ? f->filename : "");
  }
  LIBMTP_destroy_file_t(files);
}

static Entry *find_id(uint32_t id) {
  for (int i = 0; i < g_count; i++)
    if (g_entries[i].id == id)
      return &g_entries[i];
  return NULL;
}

static void build_path(uint32_t id, char *out, size_t out_len) {
  char parts[16][256];
  int n = 0;
  uint32_t cur = id;
  for (int depth = 0; depth < 16; depth++) {
    Entry *e = find_id(cur);
    if (!e)
      break;
    snprintf(parts[n++], sizeof(parts[0]), "%s", e->name);
    if (e->parent == cur || e->parent == 0 || e->parent == 0xffffffffu)
      break;
    cur = e->parent;
  }
  out[0] = 0;
  for (int i = n - 1; i >= 0; i--) {
    strncat(out, "/", out_len - strlen(out) - 1);
    strncat(out, parts[i], out_len - strlen(out) - 1);
  }
}

static int is_osmo(const char *name) {
  for (const char *p = name; *p; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    if (c == 'o' && strncasecmp(p, "osmo", 4) == 0)
      return 1;
  }
  return 0;
}

/* Current install must survive a clean pass. */
static int is_current(const char *name, const char *keep) {
  if (!keep || !keep[0])
    return 0;
  char nro[128], host[128];
  snprintf(nro, sizeof(nro), "%s.nro", keep);
  snprintf(host, sizeof(host), "%s-host.txt", keep);
  return strcmp(name, nro) == 0 || strcmp(name, host) == 0;
}

static int cmd_clean(LIBMTP_mtpdevice_t *dev, const char *keep) {
  load_entries(dev);
  int removed = 0;
  /* delete files first, then folders */
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < g_count; i++) {
      Entry *e = &g_entries[i];
      if (e->is_folder != (pass == 1))
        continue;
      if (!is_osmo(e->name))
        continue;
      if (is_current(e->name, keep)) {
        char path[512];
        build_path(e->id, path, sizeof(path));
        printf("keep %s\n", path);
        continue;
      }
      char path[512];
      build_path(e->id, path, sizeof(path));
      printf("rm 0x%08x %s %s\n", e->id, e->is_folder ? "[dir]" : "     ", path);
      if (LIBMTP_Delete_Object(dev, e->id) == 0)
        removed++;
      else {
        printf("   falhou\n");
        LIBMTP_Clear_Errorstack(dev);
      }
    }
  }
  printf("removidos: %d\n", removed);
  return 0;
}

static int cmd_push(LIBMTP_mtpdevice_t *dev, const char *nro, const char *host,
                    const char *base_name) {
  struct stat st;
  LIBMTP_Get_Storage(dev, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  uint32_t storage = 0;
  for (LIBMTP_devicestorage_t *s = dev->storage; s; s = s->next) {
    if (s->StorageDescription && strstr(s->StorageDescription, "1: SD Card")) {
      storage = s->id;
      break;
    }
  }
  if (!storage && dev->storage)
    storage = dev->storage->id;

  uint32_t parent = SWITCH_PARENT;
  LIBMTP_file_t *meta = LIBMTP_Get_Filemetadata(dev, parent);
  if (!meta) {
    fprintf(stderr, "pasta sdmc:/switch nao encontrada\n");
    return 1;
  }
  storage = meta->storage_id;
  LIBMTP_destroy_file_t(meta);

  char nro_name[128], host_name[128];
  snprintf(nro_name, sizeof(nro_name), "%s.nro", base_name);
  snprintf(host_name, sizeof(host_name), "%s-host.txt", base_name);

  const char *locals[2] = {nro, host};
  const char *remotes[2] = {nro_name, host_name};
  int rc = 0;

  /* DBI refuses to overwrite: remove same-name files in this folder first. */
  load_entries(dev);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < g_count; j++) {
      Entry *e = &g_entries[j];
      if (e->parent != parent)
        continue;
      if (strcmp(e->name, remotes[i]) != 0)
        continue;
      printf("rm antigo %s (0x%08x)\n", e->name, e->id);
      if (LIBMTP_Delete_Object(dev, e->id) != 0)
        LIBMTP_Clear_Errorstack(dev);
    }
  }

  for (int i = 0; i < 2; i++) {
    if (stat(locals[i], &st) != 0) {
      perror(locals[i]);
      rc = 1;
      continue;
    }
    LIBMTP_file_t *f = LIBMTP_new_file_t();
    f->filename = strdup(remotes[i]);
    f->filesize = (uint64_t)st.st_size;
    f->filetype = LIBMTP_FILETYPE_UNKNOWN;
    f->parent_id = parent;
    f->storage_id = storage;
    printf("send %s → sdmc:/switch/%s (%lld bytes)\n", locals[i], remotes[i],
           (long long)st.st_size);
    if (LIBMTP_Send_File_From_File(dev, locals[i], f, NULL, NULL) != 0) {
      fprintf(stderr, "  falhou\n");
      LIBMTP_Dump_Errorstack(dev);
      LIBMTP_Clear_Errorstack(dev);
      rc = 1;
    } else {
      printf("  ok\n");
    }
    LIBMTP_destroy_file_t(f);
  }
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "uso: %s clean [keep] | push <nro> <host> <name>\n", argv[0]);
    return 2;
  }
  LIBMTP_Init();
  LIBMTP_mtpdevice_t *dev = LIBMTP_Get_First_Device();
  if (!dev) {
    fprintf(stderr, "no MTP device (DBI MTP ativo? OpenMTP fechado?)\n");
    return 1;
  }
  printf("MTP connected\n");

  int rc;
  if (strcmp(argv[1], "clean") == 0) {
    rc = cmd_clean(dev, argc >= 3 ? argv[2] : "");
  } else if (strcmp(argv[1], "push") == 0 && argc >= 5) {
    rc = cmd_push(dev, argv[2], argv[3], argv[4]);
  } else {
    fprintf(stderr, "argumentos invalidos\n");
    rc = 2;
  }

  LIBMTP_Release_Device(dev);
  return rc;
}
