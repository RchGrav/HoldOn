/* Profile-hash compatibility test.
 *
 * The profile hash is a stable capability key: existing aliases, profiles and
 * sudoers grants are keyed to exactly the binary-path + argv framing in
 * hold_profile_hash_for_argv. These golden vectors fail loudly if that
 * framing ever changes (added context, reordered fields, different separators).
 * If you intend to change the hash, that is a breaking change to on-disk state;
 * update these vectors deliberately. */
#include "hold/config.h"
#include "hold/store.h"
#include <stdio.h>
#include <string.h>

struct vec {
    const char *binary_path;
    int argc;
    char **argv;
    const char *want;
};

int main(void) {
    char *a1[] = {"/bin/sleep", "60"};
    char *a2[] = {"/usr/local/bin/server", "--port", "9000", "--mode", "full"};
    char *a3[] = {"x"};
    struct vec vs[] = {
        {"/bin/sleep", 2, a1,
         "adfeec7cb96b52c70249c44528f0390a2bb14b517b47351af451ad1067524910"},
        {"/usr/local/bin/server", 5, a2,
         "a0303069a1ceca23898976944fc2800fab797da3376c1eed60bcf0271455040a"},
        {"", 0, a3,
         "ea43223103d795b68a3b38cc7deaf4bfe28bd9b330da4cee98d1fcc7abbfd736"},
    };
    size_t n = sizeof(vs) / sizeof(vs[0]);
    int fails = 0;
    for (size_t i = 0; i < n; i++) {
        char out[PROFILE_HASH_STR_LEN];
        hold_profile_hash_for_argv(vs[i].binary_path, vs[i].argc, vs[i].argv, out);
        if (strcmp(out, vs[i].want) != 0) {
            fprintf(stderr, "FAIL: binary_path='%s' argc=%d\n  got  %s\n  want %s\n",
                    vs[i].binary_path, vs[i].argc, out, vs[i].want);
            fails++;
        }
    }
    /* Collision resistance: inputs that differ must hash differently. A framing
     * that dropped the per-arg index/length separators (e.g. joined argv with a
     * single space) would collide the first pair; these lock that it does not. */
    {
        char h1[PROFILE_HASH_STR_LEN], h2[PROFILE_HASH_STR_LEN];
        char *g1a[] = {"a", "b c"};
        char *g1b[] = {"a b", "c"};
        hold_profile_hash_for_argv("/x", 2, g1a, h1);
        hold_profile_hash_for_argv("/x", 2, g1b, h2);
        if (strcmp(h1, h2) == 0) { fprintf(stderr, "COLLISION: [a,\"b c\"] == [\"a b\",c]\n"); fails++; }

        char *c2[] = {"a", "b"};
        hold_profile_hash_for_argv("/x", 2, c2, h1);
        hold_profile_hash_for_argv("/x", 1, c2, h2);
        if (strcmp(h1, h2) == 0) { fprintf(stderr, "COLLISION: argc 2 == argc 1 on same argv\n"); fails++; }

        char *c3[] = {"a"};
        hold_profile_hash_for_argv("/x", 1, c3, h1);
        hold_profile_hash_for_argv("/y", 1, c3, h2);
        if (strcmp(h1, h2) == 0) { fprintf(stderr, "COLLISION: binary_path /x == /y\n"); fails++; }
    }

    if (fails) {
        fprintf(stderr, "profile-hash vector: %d check(s) FAILED (capability key changed!)\n", fails);
        return 1;
    }
    printf("profile-hash vector: all %zu golden + 3 collision checks pass (capability key stable)\n", n);
    return 0;
}
