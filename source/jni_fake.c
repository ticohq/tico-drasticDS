/* jni_fake.c -- fake JNI environment for libdrastic_arm64.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "libc_shim.h"
#include "util.h"
#include "jni_fake.h"
#include "prefs.h"
#include "cert_data.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
//
// Every Java reference the core sees is one of these tagged heap records. The
// core only ever reads fields back off objects we built (package metadata and
// the SharedPreferences/Editor singletons), so objects carry a small array
// of named, typed fields that Get*Field looks up by the FakeID's field name.
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_BYTARR = 0x42415231, // 'BAR1'
  TAG_INTARR = 0x49415231, // 'IAR1'
  TAG_ID     = 0x4d494431, // 'MID1'
};

// a single typed field on a fake object
typedef struct {
  char name[48];
  char type;            // 'J' long, 'I' int, 'F' float, 'Z' bool, 's' string, 'o' object
  union {
    int64_t  j;
    int32_t  i;
    float    f;
    void    *o;         // child object / fake jstring
  } v;
} FakeField;

#define MAX_FIELDS 16

typedef struct {
  uint32_t tag;
  char label[96];       // class name this object was tagged with
  int nfields;
  FakeField fields[MAX_FIELDS];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

typedef struct {
  uint32_t tag;
  int len;
  uint8_t *data;        // owned; handed out by GetByteArrayElements
} FakeByteArray;

typedef struct {
  uint32_t tag;
  int len;
  int32_t *data;
} FakeIntArray;

// method and field IDs are pointers to these records; calls dispatch by name
typedef struct {
  uint32_t tag;
  char name[80];
  /* DraSticPathCache.open has an 86-byte descriptor.  Truncating it to 79
   * bytes drops the end of NativePathHandle, so the dispatcher mistakes the
   * real file-open method for an unknown Java call. */
  char sig[256];
} FakeID;

// ---------------------------------------------------------------------------
// object builders (jni_fake.h public API)
// ---------------------------------------------------------------------------

static FakeObject g_oom_obj;
static FakeString g_oom_str;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  if (!o) {
    g_oom_obj.tag = TAG_OBJECT; g_oom_obj.nfields = 0; g_oom_obj.label[0] = '\0';
    return &g_oom_obj;
  }
  o->tag = TAG_OBJECT;
  if (label) {
    size_t n = strnlen(label, sizeof(o->label) - 1);
    memcpy(o->label, label, n);
    o->label[n] = '\0';
  }
  return o;
}

void *jni_obj_new(const char *clsname) {
  return jni_make_object(clsname);
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  if (!s) {
    g_oom_str.tag = TAG_STRING; g_oom_str.utf = "";
    return &g_oom_str;
  }
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return NULL;
}

// internal accessor that never returns NULL (for logging / strlen)
static const char *obj_str(void *jstr) {
  const char *u = jni_string_utf(jstr);
  return u ? u : "";
}

// locate (or append) a named field slot on a fake object
static FakeField *obj_field(FakeObject *o, const char *field) {
  if (!o || o->tag != TAG_OBJECT)
    return NULL;
  for (int i = 0; i < o->nfields; i++)
    if (!strcmp(o->fields[i].name, field))
      return &o->fields[i];
  if (o->nfields >= MAX_FIELDS) {

    return &o->fields[MAX_FIELDS - 1];
  }
  FakeField *fld = &o->fields[o->nfields++];
  strncpy(fld->name, field, sizeof(fld->name) - 1);
  return fld;
}

void jni_obj_set_long(void *obj, const char *field, int64_t v) {
  FakeField *f = obj_field(obj, field);
  if (f) { f->type = 'J'; f->v.j = v; }
}
void jni_obj_set_int(void *obj, const char *field, int32_t v) {
  FakeField *f = obj_field(obj, field);
  if (f) { f->type = 'I'; f->v.i = v; }
}
void jni_obj_set_string(void *obj, const char *field, const char *utf) {
  FakeField *f = obj_field(obj, field);
  if (f) { f->type = 's'; f->v.o = jni_make_string(utf); }
}
void jni_obj_set_object(void *obj, const char *field, void *child) {
  FakeField *f = obj_field(obj, field);
  if (f) { f->type = 'o'; f->v.o = child; }
}

void *jni_make_byte_array(const void *data, int len) {
  FakeByteArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_BYTARR;
  a->len = len < 0 ? 0 : len;
  a->data = calloc(a->len ? a->len : 1, 1);
  if (!a->data) { a->len = 0; return a; }
  if (data && a->len)
    memcpy(a->data, data, a->len);
  return a;
}

void *jni_make_int_array(int len) {
  FakeIntArray *array = calloc(1, sizeof(*array));
  if (!array) return NULL;
  array->tag = TAG_INTARR;
  array->len = len < 0 ? 0 : len;
  array->data = calloc(array->len ? array->len : 1, sizeof(int32_t));
  if (!array->data) array->len = 0;
  return array;
}

int jni_byte_array_length(void *array) {
  FakeByteArray *bytes = array;
  return bytes && bytes->tag == TAG_BYTARR ? bytes->len : 0;
}

const uint8_t *jni_byte_array_data(void *array) {
  FakeByteArray *bytes = array;
  return bytes && bytes->tag == TAG_BYTARR ? bytes->data : NULL;
}

int jni_int_array_length(void *array) {
  FakeIntArray *ints = array;
  return ints && ints->tag == TAG_INTARR ? ints->len : 0;
}

int32_t *jni_int_array_data(void *array) {
  FakeIntArray *ints = array;
  return ints && ints->tag == TAG_INTARR ? ints->data : NULL;
}

void jni_release_byte_array(void *array) {
  FakeByteArray *bytes = array;
  if (!bytes || bytes->tag != TAG_BYTARR) return;
  bytes->tag = 0;
  free(bytes->data);
  free(bytes);
}

void jni_release_int_array(void *array) {
  FakeIntArray *ints = array;
  if (!ints || ints->tag != TAG_INTARR) return;
  ints->tag = 0;
  free(ints->data);
  free(ints);
}

void jni_release_string(void *string) {
  FakeString *value = string;
  if (!value || value == &g_oom_str || value->tag != TAG_STRING) return;
  value->tag = 0;
  free(value->utf);
  free(value);
}

void *jni_make_object_array(int n) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_OBJARR;
  a->len = n < 0 ? 0 : n;
  a->items = calloc(a->len ? a->len : 1, sizeof(void *));
  if (!a->items) a->len = 0;
  return a;
}

void jni_obj_array_set(void *arr, int i, void *elem) {
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len)
    a->items[i] = elem;
}
int jni_obj_array_len(void *arr) {
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR) ? a->len : 0;
}
void *jni_obj_array_get(void *arr, int i) {
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len)
    return a->items[i];
  return NULL;
}

// ---------------------------------------------------------------------------
// method/field ID pool: intern (name,sig) -> stable cookie. The core caches
// these once and reuses them, so dispatch only ever needs id->name.
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  if (!name) name = "";
  if (!sig) sig = "";
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {

    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->name, sizeof(id->name), "%s", name);
  snprintf(id->sig, sizeof(id->sig), "%s", sig);
  return id;
}

// ---------------------------------------------------------------------------
// shared SharedPreferences / Editor singleton
//
// The core caches one SharedPreferences handle and edit() returns an Editor it
// reuses. We back BOTH with the same fake object (the builder pattern returns
// `this`), so edit()/put*/commit all funnel into prefs.c's flat kv map.
// ---------------------------------------------------------------------------

static void *prefs_obj = NULL;   // the SharedPreferences singleton

static void *get_prefs_obj(void) {
  if (!prefs_obj)
    prefs_obj = jni_make_object("android/content/SharedPreferences");
  return prefs_obj;
}

// forward decl: dispatch_object needs to read a field back off the receiver
static void *field_object(void *obj, const char *name);

// ---------------------------------------------------------------------------
// native -> Java dispatch (by method name)
//
// Every Call*Method routes here. Receiver (obj/cls) is ignored: the cached
// FakeID name is unique enough across the contract's surface. va_args are read
// in declaration order per the cached signature (jstring/jint/jfloat/jboolean).
// Unknown calls return a benign 0/NULL so optional Android paths stay inert.
// ---------------------------------------------------------------------------

// build a Java Set-like object whose toArray() yields the '\n'-split entries
static void *stringset_for_key(const char *key) {
  const char *cur = prefs_get_string(key, "");
  // count entries
  int n = 0;
  if (cur[0]) {
    n = 1;
    for (const char *p = cur; *p; p++)
      if (*p == '\n') n++;
  }
  void *arr = jni_make_object_array(n);
  if (n) {
    char *tmp = strdup(cur);
    char *save = NULL; int i = 0;
    for (char *tok = strtok_r(tmp, "\n", &save); tok && i < n;
         tok = strtok_r(NULL, "\n", &save))
      jni_obj_array_set(arr, i++, jni_make_string(tok));
    free(tmp);
  }
  // the Set object stashes its backing array; toArray() hands it back
  void *set = jni_make_object("java/util/Set");
  jni_obj_set_object(set, "__array", arr);
  return set;
}

/* Drastic's native filesystem uses two Java-owned virtual roots.  "DraStic"
 * is the application data root: the core itself appends system/, config/,
 * etc.  Mapping it directly to SYSTEM_DIR would turn
 * DraStic/system/foo into /switch/drastic/system/system/foo. */
static int drastic_real_path(const char *virtual_path, char *output,
                             size_t output_size) {
  if (!virtual_path || !output || !output_size) return 0;
  if (!strncmp(virtual_path, "DraStic/", 8))
    snprintf(output, output_size, "%s/%s", DATA_ROOT, virtual_path + 8);
  else if (!strncmp(virtual_path, "User/", 5))
    drastic_user_path(virtual_path + 5, output, output_size);
  else if (virtual_path[0] == '/')
    snprintf(output, output_size, "%s", virtual_path);
  else {
    /* USB and SMB mounts use libnx fsdev paths such as ums0:/ and
     * nxsmb_x:/; preserve those device-qualified ROM paths. */
    const char *colon = strchr(virtual_path, ':');
    if (!colon || colon == virtual_path || colon[1] != '/') return 0;
    for (const char *cursor = virtual_path; cursor < colon; cursor++)
      if (!isalnum((unsigned char)*cursor) && *cursor != '_') return 0;
    snprintf(output, output_size, "%s", virtual_path);
  }
  for (char *cursor = output; *cursor; cursor++)
    if (*cursor == '\\') *cursor = '/';
  return 1;
}

/* The Android frontend installs system resources beside the selected DraStic
 * data directory.  The native cheat manager is the one exception to otherwise
 * immutable resources: it reopens usrcheat.dat with "rb+" to persist toggles
 * before rebuilding its active-code list.  Keep that file on the launcher's
 * documented SYSTEM_DIR path for both reads and writes.  Other resources only
 * use SYSTEM_DIR as a read-only fallback, so saves and user writes retain their
 * virtual-root locations. */
static void drastic_resource_path(const char *virtual_path, const char *mode,
                                  char *real_path,
                                  size_t real_path_size) {
  if (!virtual_path || !mode || !real_path)
    return;

  const char *relative = NULL;
  if (!strncmp(virtual_path, "DraStic/", 8))
    relative = virtual_path + 8;
  else if (!strncmp(virtual_path, "User/", 5))
    relative = virtual_path + 5;
  if (!relative || !*relative)
    return;

  if (!strcmp(relative, "usrcheat.dat")) {
    snprintf(real_path, real_path_size, "%s/usrcheat.dat", SYSTEM_DIR);
    return;
  }

  if (strpbrk(mode, "wa+"))
    return;

  struct stat status;
  if (stat(real_path, &status) == 0)
    return;

  char candidate[1024];
  snprintf(candidate, sizeof(candidate), "%s/%s", SYSTEM_DIR, relative);
  if (stat(candidate, &status) != 0 || !S_ISREG(status.st_mode))
    return;

  snprintf(real_path, real_path_size, "%s", candidate);
}

static void drastic_make_parents(const char *path) {
  char copy[1024];
  snprintf(copy, sizeof(copy), "%s", path ? path : "");
  for (char *cursor = copy + 1; *cursor; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    mkdir(copy, 0777);
    *cursor = '/';
  }
}

static void *drastic_open_path(va_list va) {
  const char *virtual_path = obj_str(va_arg(va, void *));
  const char *mode = obj_str(va_arg(va, void *));
  char real_path[1024];
  if (!drastic_real_path(virtual_path, real_path, sizeof(real_path)))
    return NULL;
  drastic_resource_path(virtual_path, mode, real_path, sizeof(real_path));
  if (strpbrk(mode, "wa+")) drastic_make_parents(real_path);
  void *handle = jni_make_object("com/dsemu/drastic/filesystem/NativePathHandle");
  jni_obj_set_string(handle, "filePath", real_path);
  jni_obj_set_int(handle, "fileFd", -1);
  const char *name = strrchr(real_path, '/');
  jni_obj_set_string(handle, "fileName", name ? name + 1 : real_path);
  return handle;
}

// -- boolean-returning calls --------------------------------------------------
static juint dispatch_boolean(const char *name, va_list va) {

  if (!strcmp(name, "rename")) {
    char source[1024], destination[1024];
    const char *virtual_source = obj_str(va_arg(va, void *));
    const char *virtual_destination = obj_str(va_arg(va, void *));
    if (!drastic_real_path(virtual_source, source, sizeof(source)) ||
        !drastic_real_path(virtual_destination, destination,
                           sizeof(destination))) return 0;
    drastic_make_parents(destination);
    /* Newer DraStic releases replace cartridge saves through
     * DraSticPathCache.rename instead of the imported POSIX rename. Route the
     * Java bridge through the same Horizon compatibility path: fsdev rename
     * cannot replace an existing destination, while Android rename can. */
    return rename_fake(source, destination) == 0;
  }
  if (!strcmp(name, "remove")) {
    char path[1024];
    const char *virtual_path = obj_str(va_arg(va, void *));
    if (!drastic_real_path(virtual_path, path, sizeof(path))) return 0;
    return remove(path) == 0 || errno == ENOENT;
  }

  // SharedPreferences.contains(key)
  if (!strcmp(name, "contains")) {
    void *jkey = va_arg(va, void *);
    const char *k = obj_str(jkey);
    int r = prefs_contains(k) ? 1 : 0;

    return r;
  }
  // SharedPreferences.getBoolean(key, default)
  if (!strcmp(name, "getBoolean")) {
    void *jkey = va_arg(va, void *);
    int def = va_arg(va, int);           // jboolean widens to int
    const char *k = obj_str(jkey);
    int r = prefs_get_bool(k, def ? true : false) ? 1 : 0;

    return r;
  }
  // Editor.commit() / apply() -> flush, true
  if (!strcmp(name, "commit") || !strcmp(name, "apply")) {
    prefs_save();
    return 1;
  }
  return 0;
}

// -- int-returning calls ------------------------------------------------------
static juint dispatch_int(const char *name, va_list va) {

  // SharedPreferences.getInt(key, default)
  if (!strcmp(name, "getInt")) {
    void *jkey = va_arg(va, void *);
    int def = va_arg(va, int);
    return (juint)(int32_t)prefs_get_int(obj_str(jkey), def);
  }
  return 0;
}

// -- long-returning calls -----------------------------------------------------
static int64_t dispatch_long(const char *name, va_list va) {

  (void)va;
  // PackageInfo.getLongVersionCode() (API 28+)
  if (!strcmp(name, "getLongVersionCode"))
    return DRASTIC_APK_VERSION_CODE;

  return 0;
}

// -- float-returning calls ----------------------------------------------------
static float dispatch_float(const char *name, va_list va) {

  // SharedPreferences.getFloat(key, default)
  if (!strcmp(name, "getFloat")) {
    void *jkey = va_arg(va, void *);
    double def = va_arg(va, double);     // jfloat promotes to double through ...
    return prefs_get_float(obj_str(jkey), (float)def);
  }
  // Display.getRefreshRate() -> Hz (we present at 60)
  if (!strcmp(name, "getRefreshRate"))
    return 60.0f;

  return 0.0f;
}

// -- object-returning calls ---------------------------------------------------
// `recv` is the receiver object/class (needed only for Set.toArray, whose
// backing array is stashed on the Set we built); NULL for static calls.
static void *dispatch_object(void *recv, const char *name, const char *sig, va_list va) {

  if (!strcmp(name, "open") && sig && strstr(sig, "NativePathHandle"))
    return drastic_open_path(va);

  // AndroidX default preferences -> the singleton
  if (!strcmp(name, "getDefaultSharedPreferences"))
    return get_prefs_obj();
  // SharedPreferences.edit() -> Editor == same singleton (builder returns this)
  if (!strcmp(name, "edit"))
    return get_prefs_obj();

  // Editor.put*(key, val) -> store, return the editor (builder pattern)
  if (!strcmp(name, "putString")) {
    void *jkey = va_arg(va, void *);
    void *jval = va_arg(va, void *);
    prefs_set_string(obj_str(jkey), obj_str(jval));
    return get_prefs_obj();
  }
  if (!strcmp(name, "putBoolean")) {
    void *jkey = va_arg(va, void *);
    int v = va_arg(va, int);
    prefs_set_bool(obj_str(jkey), v ? true : false);
    return get_prefs_obj();
  }
  if (!strcmp(name, "putInt")) {
    void *jkey = va_arg(va, void *);
    int v = va_arg(va, int);
    prefs_set_int(obj_str(jkey), v);
    return get_prefs_obj();
  }
  if (!strcmp(name, "putFloat")) {
    void *jkey = va_arg(va, void *);
    double v = va_arg(va, double);
    prefs_set_float(obj_str(jkey), (float)v);
    return get_prefs_obj();
  }
  if (!strcmp(name, "remove")) {
    void *jkey = va_arg(va, void *);
    prefs_remove(obj_str(jkey));
    return get_prefs_obj();
  }

  // getString is overloaded: SharedPreferences.getString(String,String) vs
  // Context/Resources.getString(int resId, ...). Disambiguate by signature so we
  // never deref an int resId as a jstring pointer.
  if (!strcmp(name, "getString")) {
    if (sig && !strncmp(sig, "(Ljava/lang/String;Ljava/lang/String;)", 38)) {
      void *jkey = va_arg(va, void *);
      void *jdef = va_arg(va, void *);
      const char *k = obj_str(jkey);
      const char *v = prefs_get_string(k, jdef ? jni_string_utf(jdef) : NULL);

      return v ? jni_make_string(v) : NULL;
    }
    return jni_make_string(""); // resource-string overload: no resources here
  }
  // SharedPreferences.getStringSet(key, default) -> backed Set
  if (!strcmp(name, "getStringSet")) {
    // both overloads: the key is the LAST jstring before any default Set arg.
    // 2-arg static form is (prefs, key); the instance form is (key, defSet).
    // We can't tell statically, but in both cases the entry we want is the key;
    // peel one object then treat the next as the key when it is the static form.
    void *a0 = va_arg(va, void *);
    const char *key;
    FakeObject *o = a0;
    if (o && o->tag == TAG_OBJECT) {
      // static (prefs, key): a0 was the prefs handle, next is the key jstring
      void *a1 = va_arg(va, void *);
      key = obj_str(a1);
    } else {
      // instance (key, defSet): a0 was the key jstring
      key = obj_str(a0);
    }
    return stringset_for_key(key);
  }
  // Set.toArray() -> the backing array we stashed on the Set object
  if (!strcmp(name, "toArray")) {
    void *arr = field_object(recv, "__array");
    return arr ? arr : jni_make_object_array(0);
  }

  // Context.getPackageName() -> the app id the core derives paths/ids from
  if (!strcmp(name, "getPackageName"))
    return jni_make_string("com.dsemu.drastic");
  // app-version lookup chain: getPackageManager().getPackageInfo(pkg).versionName/Code
  if (!strcmp(name, "getPackageManager"))
    return jni_make_object("android/content/pm/PackageManager");
  if (!strcmp(name, "getPackageInfo")) {
    void *pi = jni_make_object("android/content/pm/PackageInfo");
    jni_obj_set_string(pi, "versionName", "r2.6.0.4a");
    jni_obj_set_int(pi, "versionCode", DRASTIC_APK_VERSION_CODE);
    jni_obj_set_long(pi, "longVersionCode", DRASTIC_APK_VERSION_CODE);
    // anti-tamper / signature check: the core reads pi.signingInfo (API>=28) or
    // pi.signatures and bails if BOTH are null. Provide non-null objects so the
    // check proceeds; getApkContentsSigners()/toByteArray() below feed it bytes.
    jni_obj_set_object(pi, "signingInfo", jni_make_object("android/content/pm/SigningInfo"));
    void *sigs = jni_make_object_array(1);
    jni_obj_array_set(sigs, 0, jni_make_object("android/content/pm/Signature"));
    jni_obj_set_object(pi, "signatures", sigs);
    return pi;
  }
  // SigningInfo.getApkContentsSigners()/getSigningCertificateHistory() -> Signature[]
  if (!strcmp(name, "getApkContentsSigners") || !strcmp(name, "getSigningCertificateHistory")) {
    void *sigs = jni_make_object_array(1);
    jni_obj_array_set(sigs, 0, jni_make_object("android/content/pm/Signature"));
    return sigs;
  }
  // Signature.toByteArray() -> the genuine Drastic signer cert (X.509 DER,
  // extracted from the APK's META-INF). The core's anti-tamper check derives a
  // value from these bytes and compares to its (obfuscated) expected signer, so
  // it must be the REAL cert, not a dummy.
  if (!strcmp(name, "toByteArray"))
    return jni_make_byte_array(g_apk_cert, (int)g_apk_cert_len);
  if (!strcmp(name, "getApplicationInfo"))
    return jni_make_object("android/content/pm/ApplicationInfo");
  // services / resources / content the core may reflect on (chained calls on the
  // result are handled by name; rumble/etc. are no-ops). Return real objects so
  // GetObjectClass / method calls on them never deref a sentinel.
  if (!strcmp(name, "getSystemService"))
    return jni_make_object("android/os/IBinder");
  if (!strcmp(name, "getResources"))
    return jni_make_object("android/content/res/Resources");
  if (!strcmp(name, "getAssets"))
    return jni_make_object("android/content/res/AssetManager");
  if (!strcmp(name, "getContentResolver"))
    return jni_make_object("android/content/ContentResolver");
  if (!strcmp(name, "getDisplay") || !strcmp(name, "getDefaultDisplay"))
    return jni_make_object("android/view/Display");
  // Context.getApplicationContext()/getBaseContext() -> the same fake context
  if (!strcmp(name, "getApplicationContext") || !strcmp(name, "getBaseContext"))
    return recv;
  // path/data-dir helpers the core may query. getExternalFilesDir/getCacheDir/...
  // return File objects in Android; the core then calls File.getAbsolutePath().
  // We return a fake jstring tagged as the path; getAbsolutePath/getPath/toString
  // below echo it back so both shapes resolve to DATA_ROOT/cache.
  if (!strcmp(name, "getDataDirectory") || !strcmp(name, "getPublicDirectoryPath") ||
      !strcmp(name, "getExternalFilesDir") || !strcmp(name, "getFilesDir"))
    return jni_make_string(DATA_ROOT);
  if (!strcmp(name, "getCacheDir") || !strcmp(name, "getExternalCacheDir") ||
      !strcmp(name, "getCodeCacheDir"))
    return jni_make_string(CACHE_DIR);
  // File.getAbsolutePath()/getPath()/toString() -> echo the path we handed back
  // (recv is the fake jstring from the *Dir calls above), else DATA_ROOT.
  if (!strcmp(name, "getAbsolutePath") || !strcmp(name, "getPath") ||
      !strcmp(name, "getCanonicalPath") || !strcmp(name, "toString")) {
    const char *u = jni_string_utf(recv);
    return jni_make_string(u ? u : DATA_ROOT);
  }

  // Class.getName()/getSimpleName() -> the class label we stashed on the class
  // object (GetObjectClass copies the source object's label). Return it non-null
  // (a null here can break the core's error/type-name logic), Java dotted form.
  if (!strcmp(name, "getName") || !strcmp(name, "getSimpleName") ||
      !strcmp(name, "getCanonicalName")) {
    FakeObject *o = recv;
    const char *lbl = (o && o->tag == TAG_OBJECT && o->label[0]) ? o->label
                                                                 : "java/lang/Object";
    static char dotted[128];
    size_t n = 0;
    for (; lbl[n] && n < sizeof(dotted) - 1; n++) dotted[n] = (lbl[n] == '/') ? '.' : lbl[n];
    dotted[n] = '\0';

    return jni_make_string(dotted);
  }

  (void)name;
  (void)sig;
  (void)recv;
  return NULL;
}

// -- void-returning calls -----------------------------------------------------
static void dispatch_void(const char *name, va_list va) {
  (void)name;
  (void)va;
}

// ---------------------------------------------------------------------------
// field reads: pull the stored value off the fake object by the FakeID's name
// ---------------------------------------------------------------------------

static FakeField *find_field(void *obj, const char *name) {
  FakeObject *o = obj;
  if (!o || o->tag != TAG_OBJECT)
    return NULL;
  for (int i = 0; i < o->nfields; i++)
    if (!strcmp(o->fields[i].name, name))
      return &o->fields[i];
  return NULL;
}

static juint field_int(void *obj, const char *name) {
  FakeField *f = find_field(obj, name);
  if (f) return (juint)(int32_t)(f->type == 'Z' || f->type == 'I' ? f->v.i :
                                 f->type == 'J' ? (int32_t)f->v.j : 0);
  // Build$VERSION.SDK_INT is read via GetStaticIntField with this name
  if (!strcmp(name, "SDK_INT"))
    return ANDROID_SDK_INT;

  return 0;
}

static int64_t field_long(void *obj, const char *name) {
  FakeField *f = find_field(obj, name);
  if (f) return f->type == 'J' ? f->v.j : (int64_t)f->v.i;

  return 0;
}

static juint field_bool(void *obj, const char *name) {
  FakeField *f = find_field(obj, name);
  if (f) return f->v.i ? 1 : 0;
  return 0;
}

static float field_float(void *obj, const char *name) {
  FakeField *f = find_field(obj, name);
  if (f) return f->type == 'F' ? f->v.f : (float)f->v.i;
  return 0.0f;
}

static void *field_object(void *obj, const char *name) {
  FakeField *f = find_field(obj, name);
  if (f && (f->type == 'o' || f->type == 's'))
    return f->v.o;
  if (!strcmp(name, "SDK_INT"))
    return NULL;

  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function table -- slot indices match the base file / JNI ABI; new
// slots required by the contract (Part C) are added below.
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;

  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;

  return get_id(name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;

  return get_id(name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;

  FakeObject *o = obj;
  return jni_make_object((o && o->tag == TAG_OBJECT) ? o->label : "class");
}

static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

// --- NewObject (ctor) : return a fresh fake object tagged with its class -----
// Constructors used by reflected Android objects return an empty tagged object.
static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)id; (void)va;
  FakeObject *c = cls;
  return jni_make_object((c && c->tag == TAG_OBJECT) ? c->label : "object");
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = j_NewObjectV(env, cls, id, va);
  va_end(va);
  return r;
}
static void *j_NewObjectA(void *env, void *cls, FakeID *id, void *args) {
  (void)env; (void)id; (void)args;
  FakeObject *c = cls;
  return jni_make_object((c && c->tag == TAG_OBJECT) ? c->label : "object");
}

// --- Call<type>Method (instance) --------------------------------------------
static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return dispatch_boolean(id->name, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va; va_start(va, id);
  juint r = dispatch_boolean(id->name, va);
  va_end(va); return r;
}
static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env;
  return dispatch_object(obj, id->name, id->sig, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env;
  va_list va; va_start(va, id);
  void *r = dispatch_object(obj, id->name, id->sig, va);
  va_end(va); return r;
}
static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; dispatch_void(id->name, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va; va_start(va, id);
  dispatch_void(id->name, va);
  va_end(va);
}
static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return dispatch_int(id->name, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va; va_start(va, id);
  juint r = dispatch_int(id->name, va);
  va_end(va); return r;
}
static int64_t j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return dispatch_long(id->name, va);
}
static int64_t j_CallLongMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va; va_start(va, id);
  int64_t r = dispatch_long(id->name, va);
  va_end(va); return r;
}
static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; return dispatch_float(id->name, va);
}
static float j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) {
  (void)env; (void)obj;
  va_list va; va_start(va, id);
  float r = dispatch_float(id->name, va);
  va_end(va); return r;
}

// --- Call<type>StaticMethod -- share the by-name dispatchers -----------------
static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env;
  if (!id || id->tag != TAG_ID)
    return NULL;
  return dispatch_object(cls, id->name, id->sig, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env;
  if (!id || id->tag != TAG_ID)
    return NULL;
  va_list va; va_start(va, id);
  void *r = dispatch_object(cls, id->name, id->sig, va);
  va_end(va); return r;
}
static juint j_CallStaticBooleanMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return dispatch_boolean(id->name, va);
}
static juint j_CallStaticBooleanMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, id);
  juint r = dispatch_boolean(id->name, va);
  va_end(va); return r;
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; dispatch_void(id->name, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, id);
  dispatch_void(id->name, va);
  va_end(va);
}
static juint j_CallStaticIntMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return dispatch_int(id->name, va);
}
static juint j_CallStaticIntMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, id);
  juint r = dispatch_int(id->name, va);
  va_end(va); return r;
}
static int64_t j_CallStaticLongMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return dispatch_long(id->name, va);
}
static int64_t j_CallStaticLongMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, id);
  int64_t r = dispatch_long(id->name, va);
  va_end(va); return r;
}
static float j_CallStaticFloatMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; return dispatch_float(id->name, va);
}
static float j_CallStaticFloatMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, id);
  float r = dispatch_float(id->name, va);
  va_end(va); return r;
}

// --- fields ------------------------------------------------------------------
static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env; return field_object(obj, id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env; return field_int(obj, id->name);
}
static int64_t j_GetLongField(void *env, void *obj, FakeID *id) {
  (void)env; return field_long(obj, id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env; return field_bool(obj, id->name);
}
static float j_GetFloatField(void *env, void *obj, FakeID *id) {
  (void)env; return field_float(obj, id->name);
}
// GetStatic*Field: the receiver is a class cookie, not a populated object, so we
// dispatch by name only -- the one static field the core reads is SDK_INT.
static juint j_GetStaticIntField(void *env, void *cls, FakeID *id) {
  (void)env; (void)cls;
  if (!strcmp(id->name, "SDK_INT"))
    return ANDROID_SDK_INT;

  return 0;
}
static void *j_GetStaticObjectField(void *env, void *cls, FakeID *id) {
  (void)env; (void)cls;
  // android.os.Build[.VERSION] static string fields the core reads for device id
  if (!strcmp(id->name, "MANUFACTURER")) return jni_make_string(ANDROID_MANUFACTURER);
  if (!strcmp(id->name, "MODEL"))        return jni_make_string(ANDROID_MODEL);
  if (!strcmp(id->name, "DEVICE"))       return jni_make_string("Switch");
  if (!strcmp(id->name, "BRAND"))        return jni_make_string("Nintendo");
  if (!strcmp(id->name, "PRODUCT"))      return jni_make_string("Switch");
  if (!strcmp(id->name, "HARDWARE"))     return jni_make_string("nx");
  if (!strcmp(id->name, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/nx");
  if (!strcmp(id->name, "RELEASE"))      return jni_make_string("11"); // VERSION.RELEASE

  return NULL;
}

// --- strings -----------------------------------------------------------------
static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return jni_make_string(utf);
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}
static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env; return strlen(obj_str(jstr));
}
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env; return strlen(obj_str(jstr));
}

// --- object arrays -----------------------------------------------------------
static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR) return a->len;
  FakeByteArray *b = arr;
  if (b && b->tag == TAG_BYTARR) return b->len;
  FakeIntArray *ints = arr;
  if (ints && ints->tag == TAG_INTARR) return ints->len;
  return 0;
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env; return jni_obj_array_get(arr, idx);
}
static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env; jni_obj_array_set(arr, idx, val);
}
static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  void *a = jni_make_object_array(len);
  if (init)
    for (int i = 0; i < len; i++) jni_obj_array_set(a, i, init);
  return a;
}

// --- byte arrays -------------------------------------------------------------
static void *j_NewByteArray(void *env, int len) {
  (void)env; return jni_make_byte_array(NULL, len);
}
static void *j_GetByteArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakeByteArray *b = arr;
  return (b && b->tag == TAG_BYTARR) ? b->data : NULL;
}
static void j_ReleaseByteArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode; // elements alias our buffer
}
static void j_GetByteArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakeByteArray *b = arr;
  if (b && b->tag == TAG_BYTARR && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(buf, b->data + start, len);
}
static void j_SetByteArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakeByteArray *b = arr;
  if (b && b->tag == TAG_BYTARR && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(b->data + start, buf, len);
}

// --- int arrays / critical primitive access ---------------------------------
static void *j_NewIntArray(void *env, int len) {
  (void)env; return jni_make_int_array(len);
}
static void *j_GetIntArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  return jni_int_array_data(arr);
}
static void j_ReleaseIntArrayElements(void *env, void *arr, void *elements,
                                      int mode) {
  (void)env; (void)arr; (void)elements; (void)mode;
}
static void j_GetIntArrayRegion(void *env, void *arr, int start, int len,
                                void *buffer) {
  (void)env;
  FakeIntArray *ints = arr;
  if (ints && ints->tag == TAG_INTARR && start >= 0 && len >= 0 &&
      start + len <= ints->len)
    memcpy(buffer, ints->data + start, (size_t)len * sizeof(int32_t));
}
static void j_SetIntArrayRegion(void *env, void *arr, int start, int len,
                                const void *buffer) {
  (void)env;
  FakeIntArray *ints = arr;
  if (ints && ints->tag == TAG_INTARR && start >= 0 && len >= 0 &&
      start + len <= ints->len)
    memcpy(ints->data + start, buffer, (size_t)len * sizeof(int32_t));
}
static void *j_GetPrimitiveArrayCritical(void *env, void *arr,
                                         uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  if (!arr) return NULL;
  const uint32_t tag = *(const uint32_t *)arr;
  if (tag == TAG_INTARR) return ((FakeIntArray *)arr)->data;
  if (tag == TAG_BYTARR) return ((FakeByteArray *)arr)->data;
  return NULL;
}
static void j_ReleasePrimitiveArrayCritical(void *env, void *arr,
                                            void *elements, int mode) {
  (void)env; (void)arr; (void)elements; (void)mode;
}

// --- misc --------------------------------------------------------------------
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n;

  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) {
  (void)env; if (vm) *vm = fake_vm; return JNI_OK;
}
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void_1(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

static juint j_unimplemented(void) {

  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (slot indices per the JNI specification; same numbering as the
// base file -- extra slots added for the Android core surface)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (env) *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version;
  if (env) *env = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void_1; // ExceptionDescribe
  env_table[17]  = (void *)j_void_1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef; // DeleteGlobalRef
  env_table[23]  = (void *)j_DeleteRef; // DeleteLocalRef
  env_table[24]  = (void *)j_ret0_3;    // IsSameObject
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_ret0_2;    // EnsureLocalCapacity
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[30]  = (void *)j_NewObjectA;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID; // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;          // GetStaticFieldID
  env_table[145] = (void *)j_GetStaticObjectField;
  env_table[146] = (void *)j_GetBooleanField;     // GetStaticBooleanField (rare)
  env_table[150] = (void *)j_GetStaticIntField;
  // slot 163 is NewString (jchar*,len) -- the core uses NewStringUTF, so leave
  // 163 as j_unimplemented rather than mis-binding it to GetStringLength.
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;        // 175 is NewBooleanArray; 176 is NewByteArray
  env_table[179] = (void *)j_NewIntArray;
  env_table[184] = (void *)j_GetByteArrayElements;
  env_table[187] = (void *)j_GetIntArrayElements;
  env_table[192] = (void *)j_ReleaseByteArrayElements;
  env_table[195] = (void *)j_ReleaseIntArrayElements;
  env_table[200] = (void *)j_GetByteArrayRegion;
  env_table[203] = (void *)j_GetIntArrayRegion;
  env_table[208] = (void *)j_SetByteArrayRegion;
  env_table[211] = (void *)j_SetIntArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2; // UnregisterNatives
  env_table[217] = (void *)j_ret0_2; // MonitorEnter
  env_table[218] = (void *)j_ret0_2; // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPrimitiveArrayCritical;
  env_table[223] = (void *)j_ReleasePrimitiveArrayCritical;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
