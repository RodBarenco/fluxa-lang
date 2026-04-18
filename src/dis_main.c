/* dis_main.c — standalone fluxa_dis binary entry point
 * The implementation lives in dis.c, included via Makefile.
 * fluxa binary links dis.c directly (fluxa_dis_file symbol).
 */
#include "dis.c"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fluxa_dis <file.flx> [-o out.txt]\n");
        return 1;
    }
    const char *inpath  = argv[1];
    const char *outpath = NULL;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
    return fluxa_dis_file(inpath, outpath);
}
