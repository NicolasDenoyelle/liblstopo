#include <private/autogen/config.h>
#include "lstopo.h"
#include "hwloc.h"

#include <unistd.h>

#define BLOCK 1

#ifdef HWLOC_WIN_SYS
#define hwloc_sleep(x) Sleep(x*1000)
#else
#define hwloc_sleep sleep
#endif

static int callback(struct lstopo_output *loutput, hwloc_obj_t obj, unsigned depth, unsigned x, unsigned width, unsigned y, unsigned height)
{
  struct lstopo_obj_userdata *lud = obj->userdata;
  struct draw_methods *methods = loutput->methods;
  unsigned fontsize = loutput->fontsize;
  unsigned gridsize = loutput->gridsize;

  switch (obj->type) {
  case HWLOC_OBJ_PACKAGE:
    methods->box(loutput, 0xff, 0, 0, depth, x, width, y, height);
    methods->text(loutput, 0, 0, 0, fontsize, depth, x+gridsize, y+gridsize, "toto package");
    return 0;
  case HWLOC_OBJ_CORE:
    methods->box(loutput, 0, 0xff, 0, depth, x, width, y, height);
    methods->text(loutput, 0, 0, 0, fontsize, depth, x+gridsize, y+gridsize, "titi core");
    return 0;
  case HWLOC_OBJ_PU:
    methods->box(loutput, 0, 0, 0xff, depth, x, width, y, height);
    methods->text(loutput, 0, 0, 0, fontsize, depth, x+gridsize, y+gridsize, "tutu pu");
    return 0;
  case HWLOC_OBJ_NUMANODE:
    methods->box(loutput, 0xd2, 0xe7, 0xa4, depth, x, width, y, height);
    methods->box(loutput, 0xff, 0, 0xff, depth, x+gridsize, width-2*gridsize, y+gridsize, fontsize+2*gridsize);
    methods->text(loutput, 0, 0, 0, fontsize, depth, x+2*gridsize, y+2*gridsize, "numanuma");
    return 0;
  case HWLOC_OBJ_L1CACHE:
  case HWLOC_OBJ_L2CACHE:
  case HWLOC_OBJ_L3CACHE:
  case HWLOC_OBJ_L4CACHE:
  case HWLOC_OBJ_L5CACHE:
  case HWLOC_OBJ_L1ICACHE:
  case HWLOC_OBJ_L2ICACHE:
  case HWLOC_OBJ_L3ICACHE:
    methods->box(loutput, 0xff, 0xff, 0, depth, x, width, y, height);
    methods->text(loutput, 0, 0, 0, fontsize, depth, x+gridsize, y+gridsize, "$$$$$");
    return 0;
  default:
    return -1;
  }
}

int main(int argc, char *argv[])
{
  hwloc_topology_t topology;
  struct lstopo_output loutput;

  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);

  lstopo_init(&loutput);
  loutput.logical = 0;
  loutput.topology = topology;
  loutput.drawing_callback = callback;
  lstopo_prepare(&loutput);

  output_x11(&loutput, NULL);

#if BLOCK
  if (loutput.methods && loutput.methods->iloop)
    loutput.methods->iloop(&loutput, 1);
#else // non-block
  if (loutput.methods && loutput.methods->iloop) {
    while (loutput.methods->iloop(&loutput, 0) >= 0) {
      printf("sleeping 1s\n");
      hwloc_sleep(1);
    }
  }
#endif

  if (loutput.methods && loutput.methods->end)
    loutput.methods->end(&loutput);

  lstopo_destroy(&loutput);

  hwloc_topology_destroy (topology);
  return EXIT_SUCCESS;
}
