// cgzinfo.cpp: show info about and contents of cgz files

#include "cube.h"

sqr *world = NULL;
int sfactor, ssize, cubicsize, mipsize;
header hdr;
vector<entity> ents;

void setupworld(int factor)
{
    ssize = 1<<(sfactor = factor);
    cubicsize = ssize*ssize;
    mipsize = cubicsize*134/100;
    world = new sqr[mipsize];
    memset(world, 0, mipsize*sizeof(sqr));
}

void sqrdefault(sqr *s)
{
    if(!s) return;
    s->ftex = DEFAULT_FLOOR;
    s->ctex = DEFAULT_CEIL;
    s->wtex = s->utex = DEFAULT_WALL;
    s->type = SOLID;
    s->floor = 0;
    s->ceil = 16;
    s->vdelta = s->defer = s->tag = 0;
}

void rldecodecubes(ucharbuf &f, sqr *s, int len, int version, bool silent) // run-length decoding of a series of cubes (version is only relevant, if < 6)
{
    sqr *t = NULL, *e = s + len;
    while(s < e)
    {
        int type = f.overread() ? -1 : f.get();
        switch(type)
        {
            case -1:
            {
                if(!silent) printf("while reading map at %ld: unexpected end of file\n", cubicsize - (e - s));
                f.forceoverread();
                silent = true;
                sqrdefault(s);
                break;
            }
            case 255:
            {
                if(!t) { f.forceoverread(); continue; }
                int n = f.get();
                loopi(n) memcpy(s++, t, sizeof(sqr));
                s--;
                break;
            }
            case 254: // only in MAPVERSION<=2
            {
                if(!t) { f.forceoverread(); continue; }
                memcpy(s, t, sizeof(sqr));
                f.get(); f.get();
                break;
            }
            case SOLID:
            {
                sqrdefault(s);                  // takes care of ftex, ctex, floor, ceil and tag
                s->type = SOLID;
                s->utex = s->wtex = f.get();
                s->vdelta = f.get();
                if(version<=2) { f.get(); f.get(); }
                break;
            }
            case 253: // SOLID with all textures during editing (undo)
                type = SOLID;
            default:
            {
                if(type<0 || type>=MAXTYPE)
                {
                    if(!silent) printf("while reading map at %ld: type %d out of range\n", cubicsize - (e - s), type);
                    f.overread();
                    continue;
                }
                sqrdefault(s);
                s->type = type;
                s->floor = f.get();
                s->ceil = f.get();
                if(s->floor>=s->ceil) s->floor = s->ceil-1;  // for pre 12_13
                s->wtex = f.get();
                s->ftex = f.get();
                s->ctex = f.get();
                if(version<=2) { f.get(); f.get(); }
                s->vdelta = f.get();
                s->utex = (version>=2) ? f.get() : s->wtex;
                s->tag = (version>=5) ? f.get() : 0;
            }
        }
        s->defer = 0;
        t = s;
        s++;
    }
}

int fixmapheadersize(int version, int headersize)
{
    if(version < 4) return sizeof(header) - sizeof(int) * 16;
    else if(version == 7 || version == 8) return sizeof(header) + sizeof(char) * 128;  // mediareq
    else if(version < 10 || headersize < int(sizeof(header))) return sizeof(header);
    return headersize;
}

uchar *extrabuf = NULL;
int extrasize = 0;

bool load_world(char *mname)
{
    const int sizeof_header = sizeof(header), sizeof_baseheader = sizeof_header - sizeof(int) * 16;
    gzFile f = mname ? gzopen(mname, "rb") : gzdopen(STDIN_FILENO, "rb");
    if(!mname) mname = (char *) "<stdin>";
    if(!f) { printf("could not read map %s\n", mname); return false; }
    header tmp;
    memset(&tmp, 0, sizeof_header);
    if(gzread(f, &tmp, sizeof_baseheader) != sizeof_baseheader ||
       (strncmp(tmp.head, "CUBE", 4)!=0 && strncmp(tmp.head, "ACMP",4)!=0)) { printf("while reading map: header malformatted (1)\n"); gzclose(f); return false; }
    lilswap(&tmp.version, 4); // version, headersize, sfactor, numents
    if(tmp.version > MAXMAPVERSION) { printf("this map requires a newer version of AssaultCube\n"); gzclose(f); return false; }
    if(tmp.sfactor<SMALLEST_FACTOR || tmp.sfactor>LARGEST_FACTOR || tmp.numents > MAXENTITIES) { printf("\f3illegal map size\n"); gzclose(f); return false; }
    tmp.headersize = fixmapheadersize(tmp.version, tmp.headersize);
    int restofhead = min(tmp.headersize, sizeof_header) - sizeof_baseheader;
    if(gzread(f, &tmp.waterlevel, restofhead) != restofhead) { printf("while reading map: header malformatted (2)\n"); gzclose(f); return false; }
    if(tmp.headersize > sizeof_header)
    {
        extrasize = tmp.headersize - sizeof_header;
        if(tmp.version < 9) extrasize = 0;  // throw away mediareq...
        else if(extrasize > MAXHEADEREXTRA) extrasize = MAXHEADEREXTRA;
        if(extrasize)
        { // map file actually has extra header data that we want too preserve
            extrabuf = new uchar[extrasize];
            if(gzread(f, extrabuf, extrasize) != extrasize) { printf("while reading map: header malformatted (3)\n"); gzclose(f); return false; }
        }
        gzseek(f, tmp.headersize, SEEK_SET);
    }
    hdr = tmp;
    lilswap(&hdr.waterlevel, 1);
    lilswap(&hdr.maprevision, 2);

    loopi(hdr.numents)
    {
        persistent_entity &e = ents.add();
        gzread(f, &e, sizeof(persistent_entity));
        lilswap((short *)&e, 4);
    }
    delete[] world;
    setupworld(hdr.sfactor);

    vector<uchar> rawcubes; // fetch whole file into buffer
    loopi(9)
    {
        ucharbuf q = rawcubes.reserve(cubicsize);
        q.len = gzread(f, q.buf, cubicsize);
        rawcubes.addbuf(q);
        if(q.len < cubicsize) break;
    }
    ucharbuf uf(rawcubes.getbuf(), rawcubes.length());
    rldecodecubes(uf, world, cubicsize, hdr.version, false); // decode file

    gzclose(f);
    return true;
}

void printdump(const char *prefix, uchar *s, int len, bool addascii)
{
    int perline = addascii ? 16 : 32;
    while(len > 0)
    {
        printf("%s: ", prefix);
        uchar *l = s;
        int ll = 0;
        loopi(perline)
        {
            if(len > 0)
            {
                printf("%02X", *s++);
                ll++;
            }
            else printf("  ");
            len--;
        }
        if(addascii)
        {
            printf("  ");
            loopi(ll) printf("%c", isprint(l[i]) ? l[i] : '.');
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    bool dumpgeometry = false, listents = false, notextures = false;
    char *filename = NULL;
    loopi(argc)
    {
        if(!strcmp(argv[i], "-e")) listents = true;
        else if(!strcmp(argv[i], "-g")) dumpgeometry = true;
        else if(!strcmp(argv[i], "-t")) notextures = true;
        else if(i) filename = argv[i];  // last one wins
    }
    if(load_world(filename))
    {
        printf("AC-MAP (%02X%02X%02X%02X) %c%c%c%c\n", hdr.head[0], hdr.head[1], hdr.head[2],
            hdr.head[3], isalpha(hdr.head[0]) ? hdr.head[0] : '#', isalpha(hdr.head[1]) ? hdr.head[1] : '#', isalpha(hdr.head[2]) ? hdr.head[2] : '#', isalpha(hdr.head[3]) ? hdr.head[3] : '#');
        printf("version: %d\n"
               "headersize: %d\n"
               "sfactor: %d\n"
               "numents: %d\n"
               "waterlevel: %d\n"
               "watercolor: %d %d %d %d\n"
               "maprevision: %d\n"
               "ambient: 0x%06X\n",
               hdr.version, hdr.headersize, hdr.sfactor, hdr.numents, hdr.waterlevel, hdr.watercolor[0], hdr.watercolor[1], hdr.watercolor[2], hdr.watercolor[3], hdr.maprevision, hdr.ambient);
        printdump("maptitle", (uchar *) hdr.maptitle, 128, true);
        printdump("texlists", (uchar *) hdr.texlists, 3*256, false);
        printdump("reserved", (uchar *) hdr.reserved, sizeof(int) * 12, false);
        if(extrasize && extrabuf) printdump("headerextra", extrabuf, extrasize, true);
        const char *entnames[] = { "none?", "light", "playerstart", "pistol", "ammobox","grenades", "health", "helmet", "armour", "akimbo", "mapmodel", "trigger", "ladder", "ctf-flag", "sound", "clip", "plclip" };
        if(listents) loopv(ents)
        {
            entity &e = ents[i];
            printf("entity: %d %d|%d|%d  %d %d %d %d %s\n", e.type, e.x, e.y, e.z, e.attr1, e.attr2, e.attr3, e.attr4, e.type >= 0 && e.type < MAXENTTYPES ? entnames[e.type] : "unknown");
        }
        const char *cubetypes[] = {"SOLID", "CORNER", "FHF", "CHF", "SPACE"};
        if(dumpgeometry) loopi(cubicsize)
        {
            sqr &s = world[i];
            const char *cubetype = s.type >= 0 && s.type < SEMISOLID ? cubetypes[s.type] : "unknown";
            if(notextures) printf("cube: type(%d) %d-%d vdelta(%d) tag(0x%02X)  %s\n", s.type, s.floor, s.ceil, s.vdelta, s.tag, cubetype);
            else printf("cube: type(%d) %d-%d w(%d) f(%d) c(%d) u(%d) vdelta(%d) tag(0x%02X)  %s\n", s.type, s.floor, s.ceil, s.wtex, s.ftex, s.ctex, s.utex, s.vdelta, s.tag, cubetype);
        }
    }
    return EXIT_SUCCESS;
}

