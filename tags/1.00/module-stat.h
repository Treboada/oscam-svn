#include "globals.h"
#include "module-obj-llist.h"

LLIST *reader_stat[CS_MAXREADER];
static int stat_load_save = 0;

extern struct s_reader *reader;

void init_stat();

READER_STAT *get_stat(int ridx, ushort caid, ulong prid, ushort srvid);

int remove_stat(int ridx, ushort caid, ulong prid, ushort srvid);

void calc_stat(READER_STAT *stat);

void add_stat(int ridx, ushort caid, ulong prid, ushort srvid, int time, int rc);

void add_reader_stat(ADD_READER_STAT *stat);

int get_best_reader(GET_READER_STAT *grs, int *result);

void clear_reader_stat(int ridx);
