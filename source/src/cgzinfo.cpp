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
    if(tmp.version > MAPVERSION) { printf("this map requires a newer version of AssaultCube\n"); gzclose(f); return false; }
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
    lilswap(&hdr.maprevision, 4); // maprevision, ambient, flags, timestamp
    bool oldentityformat = hdr.version < 10; // version < 10 have only 4 attributes and no scaling

    loopi(hdr.numents)
    {
        persistent_entity &e = ents.add();
        gzread(f, &e, oldentityformat ? 12 : sizeof(persistent_entity));
        lilswap((short *)&e, 4);
        if(!oldentityformat) lilswap(&e.attr5, 1);
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

const char *time2string(int itime)
{
    static string asciitime;
    time_t t = (time_t) itime;
    struct tm *timeinfo;
    timeinfo = gmtime (&t);
    strftime(asciitime, sizeof(string) - 1, "%c", timeinfo);
    return asciitime;
}

uchar entscale[MAXENTTYPES][7] =
{ // (no zeros allowed here!)
    {  1,  1,  1,  1,  1,  1,  1 },  // deleted
    {  1,  1,  1,  1,  1,  1,  1 },  // light
    { 10,  1,  1,  1,  1,  1,  1 },  // playerstart
    { 10,  1,  1,  1,  1,  1,  1 },  // pistol
    { 10,  1,  1,  1,  1,  1,  1 },  // ammobox
    { 10,  1,  1,  1,  1,  1,  1 },  // grenades
    { 10,  1,  1,  1,  1,  1,  1 },  // health
    { 10,  1,  1,  1,  1,  1,  1 },  // helmet
    { 10,  1,  1,  1,  1,  1,  1 },  // armour
    { 10,  1,  1,  1,  1,  1,  1 },  // akimbo
    { 10,  1,  5,  1, 10,  1,  1 },  // mapmodel
    {  1,  1,  1,  1,  1,  1,  1 },  // trigger
    {  1,  1,  1,  1,  1,  1,  1 },  // ladder
    { 10,  1,  1,  1,  1,  1,  1 },  // ctf-flag
    {  1,  1,  1,  1,  1,  1,  1 },  // sound
    { 10,  5,  5,  5,  1, 10,  1 },  // clip
    { 10,  5,  5,  5,  1, 10,  1 }   // plclip
};

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
        printf("AC-MAP (%02X%02X%02X%02X) %c%c%c%c  (cgzinfo version 0.2)\n", hdr.head[0], hdr.head[1], hdr.head[2],
            hdr.head[3], isalpha(hdr.head[0]) ? hdr.head[0] : '#', isalpha(hdr.head[1]) ? hdr.head[1] : '#', isalpha(hdr.head[2]) ? hdr.head[2] : '#', isalpha(hdr.head[3]) ? hdr.head[3] : '#');
        printf("version: %d\n"
               "headersize: %d\n"
               "sfactor: %d\n"
               "numents: %d\n"
               "waterlevel: %g\n"
               "watercolor: %d %d %d %d\n"
               "maprevision: %d\n"
               "ambient: 0x%06X\n"
               "flags: 0x%08X\n"
               "timestamp: %d (%s)\n",
               hdr.version,
               hdr.headersize,
               hdr.sfactor,
               hdr.numents,
               float(hdr.waterlevel) / (hdr.version >= 10 ? 10 : 1),
               hdr.watercolor[0], hdr.watercolor[1], hdr.watercolor[2], hdr.watercolor[3],
               hdr.maprevision,
               hdr.ambient,
               hdr.flags,
               hdr.timestamp, time2string(hdr.timestamp));
        printdump("maptitle", (uchar *) hdr.maptitle, 128, true);
        printdump("texlists", (uchar *) hdr.texlists, 3*256, false);
        printdump("reserved", (uchar *) hdr.reserved, sizeof(int) * 10, false);
        if(extrasize && extrabuf) printdump("headerextra", extrabuf, extrasize, true);
        const char *entnames[] = { "none?", "light", "playerstart", "pistol", "ammobox","grenades", "health", "helmet", "armour", "akimbo", "mapmodel", "trigger", "ladder", "ctf-flag", "sound", "clip", "plclip" };
        if(listents) loopv(ents)
        {
            entity &e = ents[i];
            int t = e.type < MAXENTTYPES ? e.type : NOTUSED;
            if(hdr.version < 10) t = NOTUSED; // -> unscaled
            printf("entity: %d %d|%d|%d  %g %g %g %g", e.type, e.x, e.y, e.z, float(e.attr1) / entscale[t][0], float(e.attr2) / entscale[t][1], float(e.attr3) / entscale[t][2], float(e.attr4) / entscale[t][3]);
            if(hdr.version >= 10) printf(" %g %g %g", float(e.attr5) / entscale[t][4], float(e.attr6) / entscale[t][5], float(e.attr7) / entscale[t][6]);
            printf(" %s\n", e.type < MAXENTTYPES ? entnames[e.type] : "unknown");
        }
        const char *cubetypes[] = {"SOLID", "CORNER", "FHF", "CHF", "SPACE", "SEMISOLID" };
        // geometry statistics first
        int usedtypes[256] = { 0 }, usedtexs[256] = { 0 }, usedheights[256] = { 0 }, usedvdeltas[256] = { 0 }, usedtags[256] = { 0 }, usedtagclips = 0, usedtagplclips = 0;
        short layout[32 * 32] = { 0 };
        int reduct = sfactor - 5, smask = ssize - 1;
        sqr *s = world;
        loopi(cubicsize)
        {
            usedtypes[s->type]++;
            usedtexs[s->wtex]++;
            if(s->type != SOLID)
            {
                usedtexs[s->utex]++;
                usedtexs[s->ftex]++;
                usedtexs[s->ctex]++;
                usedheights[clamp(s->ceil - s->floor, 0, 255)]++;
                if(s->tag)
                {
                    if(s->tag & TAGTRIGGERMASK) usedtags[s->tag & TAGTRIGGERMASK]++;
                    if(s->tag & TAGCLIP) usedtagclips++;
                    else if(s->tag & TAGPLCLIP) usedtagplclips++;
                }
                layout[((i & smask) >> reduct) + 32 * (i >> (2 * sfactor - 5))]++;
            }
            usedvdeltas[s->vdelta]++;
            s++;
        }
        reduct = 2 * reduct - 2;
        int offs = (1 << reduct) - 1;
        loopi(32 * 32)
        {
            if(!(i & ~768) && i) printf("maplayout: -------- -------- -------- --------\n");
            if(!(i & 31)) printf("maplayout: ");
            printf("%c", "#*+  "[(layout[i] + offs) >> reduct]);
            if((i & 7) == 7) printf("%c", (i & 31) == 31 ? '\n' : '|');
        }
        int unknowntypes = cubicsize;
        loopi(MAXTYPE) unknowntypes -= usedtypes[i];
        printf("total cubes: %d\n", cubicsize);
        loopi(MAXTYPE) printf("%s cubes: %d\n", cubetypes[i], usedtypes[i]);
        printf("cubes of unknown type: %d\n", unknowntypes);
        printf("tag clipped cubes: %d\n"
               "tag plclipped cubes: %d\n",
               usedtagclips,
               usedtagplclips);
        int minheight = 255, maxheight = 0, maxvdelta = 0;
        loopi(256) if(usedheights[i])
        {
            if(i < minheight) minheight = i;
            if(i > maxheight) maxheight = i;
        }
        loopi(maxheight - minheight + 1) printf("cubes of height %d: %d\n", i, usedheights[minheight + i]);
        loopi(256) if(usedvdeltas[i] && i > maxvdelta) maxvdelta = i;
        loopi(maxvdelta + 1) printf("cubes with vdelta %d: %d\n", i, usedvdeltas[i]);
        loopi(256) if(usedtags[i]) printf("cubes with tag %d: %d\n", i, usedtags[i]);
        loopi(256) if(usedtexs[i]) printf("uses of texture slot #%d: %d\n", i, usedtexs[i]);
        s = world;
        if(dumpgeometry) loopi(cubicsize) // fully detailed geometry dump
        {
            const char *cubetype = s->type < SEMISOLID ? cubetypes[s->type] : "unknown";
            if(notextures) printf("cube: type(%d) %d-%d vdelta(%d) tag(0x%02X)  %s\n", s->type, s->floor, s->ceil, s->vdelta, s->tag, cubetype);
            else printf("cube: type(%d) %d-%d w(%d) f(%d) c(%d) u(%d) vdelta(%d) tag(0x%02X)  %s\n", s->type, s->floor, s->ceil, s->wtex, s->ftex, s->ctex, s->utex, s->vdelta, s->tag, cubetype);
            s++;
        }
    }
    return EXIT_SUCCESS;
}

