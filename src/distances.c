/*
 * Copyright © 2010 INRIA
 * See COPYING in top-level directory.
 */

#include <private/config.h>
#include <hwloc.h>
#include <private/private.h>
#include <private/debug.h>

#include <float.h>

/* called during topology init */
void hwloc_topology_distances_init(struct hwloc_topology *topology)
{
  unsigned i;
  for (i=0; i < HWLOC_OBJ_TYPE_MAX; i++) {
    /* no distances yet */
    topology->os_distances[i].nbobjs = 0;
    topology->os_distances[i].objs = NULL;
    topology->os_distances[i].indexes = NULL;
    topology->os_distances[i].distances = NULL;
  }
}

/* called when reloading a topology.
 * keep initial parameters (from set_distances and environment),
 * but drop what was generated during previous load().
 */
void hwloc_topology_distances_clear(struct hwloc_topology *topology)
{
  unsigned i;
  for (i=0; i < HWLOC_OBJ_TYPE_MAX; i++) {
    /* remove final distance matrices, but keep physically-ordered ones */
    free(topology->os_distances[i].objs);
    topology->os_distances[i].objs = NULL;
  }
}

/* called during topology destroy */
void hwloc_topology_distances_destroy(struct hwloc_topology *topology)
{
  unsigned i;
  for (i=0; i < HWLOC_OBJ_TYPE_MAX; i++) {
    /* remove final distance matrics AND physically-ordered ones */
    free(topology->os_distances[i].indexes);
    topology->os_distances[i].indexes = NULL;
    free(topology->os_distances[i].objs);
    topology->os_distances[i].objs = NULL;
    free(topology->os_distances[i].distances);
    topology->os_distances[i].distances = NULL;
  }
}

/* check a distance index array and matrix and insert it in the topology.
 * the caller gives us those pointers, we take care of freeing them later and so on.
 */
static int hwloc_topology__set_distance_matrix(hwloc_topology_t __hwloc_restrict topology, hwloc_obj_type_t type,
					       unsigned nbobjs, unsigned *indexes, float *distances)
{
  unsigned i,j;

  /* make sure we don't have the same index twice */
  for(i=0; i<nbobjs; i++)
    for(j=i+1; j<nbobjs; j++)
      if (indexes[i] == indexes[j]) {
	free(indexes);
	free(distances);
	errno = EINVAL;
	return -1;
      }

  free(topology->os_distances[type].indexes);
  free(topology->os_distances[type].distances);
  topology->os_distances[type].nbobjs = nbobjs;
  topology->os_distances[type].indexes = indexes;
  topology->os_distances[type].distances = distances;
  return 0;
}

static hwloc_obj_t hwloc_find_obj_by_type_and_os_index(hwloc_obj_t root, hwloc_obj_type_t type, unsigned os_index)
{
  hwloc_obj_t child;
  if (root->type == type && root->os_index == os_index)
    return root;
  child = root->first_child;
  while (child) {
    hwloc_obj_t found = hwloc_find_obj_by_type_and_os_index(child, type, os_index);
    if (found)
      return found;
    child = child->next_sibling;
  }
  return NULL;
}

static void hwloc_get_type_distances_from_string(struct hwloc_topology *topology,
						 hwloc_obj_type_t type, char *string)
{
  /* the string format is: "index[0],...,index[N-1]:distance[0],...,distance[N*N-1]"
   * or "index[0],...,index[N-1]:X*Y" or "index[0],...,index[N-1]:X*Y*Z"
   */
  char *tmp = string, *next;
  unsigned *indexes;
  float *distances;
  unsigned nbobjs = 0, i, j, x, y, z;

  /* count indexes */
  while (1) {
    size_t size = strspn(tmp, "0123456789");
    if (tmp[size] != ',') {
      /* last element */
      tmp += size;
      nbobjs++;
      break;
    }
    /* another index */
    tmp += size+1;
    nbobjs++;
  }

  if (*tmp != ':') {
    fprintf(stderr, "Ignoring %s distances from environment variable, missing colon\n",
	    hwloc_obj_type_string(type));
    return;
  }

  indexes = calloc(nbobjs, sizeof(unsigned));
  distances = calloc(nbobjs*nbobjs, sizeof(float));
  tmp = string;

  /* parse indexes */
  for(i=0; i<nbobjs; i++) {
    indexes[i] = strtoul(tmp, &next, 0);
    tmp = next+1;
  }

  /* parse distances */
  z=1; /* default if sscanf finds only 2 values below */
  if (sscanf(tmp, "%u*%u*%u", &x, &y, &z) >= 2) {
    /* generate the matrix to create x groups of y elements */
    if (x*y*z != nbobjs) {
      fprintf(stderr, "Ignoring %s distances from environment variable, invalid grouping (%u*%u*%u=%u instead of %u)\n",
	      hwloc_obj_type_string(type), x, y, z, x*y*z, nbobjs);
      free(indexes);
      free(distances);
      return;
    }
    for(i=0; i<nbobjs; i++)
      for(j=0; j<nbobjs; j++)
	if (i==j)
	  distances[i*nbobjs+j] = 1;
	else if (i/z == j/z)
	  distances[i*nbobjs+j] = 2;
	else if (i/z/y == j/z/y)
	  distances[i*nbobjs+j] = 4;
	else
	  distances[i*nbobjs+j] = 8;

  } else {
    /* parse a comma separated list of distances */
    for(i=0; i<nbobjs*nbobjs; i++) {
      distances[i] = strtof(tmp, &next);
      tmp = next+1;
      if (!*next && i!=nbobjs*nbobjs-1) {
	fprintf(stderr, "Ignoring %s distances from environment variable, not enough values (%u out of %u)\n",
		hwloc_obj_type_string(type), i+1, nbobjs*nbobjs);
	free(indexes);
	free(distances);
	return;
      }
    }
  }

  if (hwloc_topology__set_distance_matrix(topology, type, nbobjs, indexes, distances) < 0)
    fprintf(stderr, "Ignoring invalid %s distances from environment variable\n", hwloc_obj_type_string(type));
}

/* take distances in the environment, store them as is in the topology.
 * we'll convert them into object later once the tree is filled
 */
void hwloc_store_distances_from_env(struct hwloc_topology *topology)
{
  hwloc_obj_type_t type;
  for(type = HWLOC_OBJ_SYSTEM; type < HWLOC_OBJ_TYPE_MAX; type++) {
    char envname[64];
    snprintf(envname, sizeof(envname), "HWLOC_%s_DISTANCES", hwloc_obj_type_string(type));
    char *env = getenv(envname);
    if (env)
      hwloc_get_type_distances_from_string(topology, type, env);
  }
}

/* take the given distance, store them as is in the topology.
 * we'll convert them into object later once the tree is filled.
 */
int hwloc_topology_set_distance_matrix(hwloc_topology_t __hwloc_restrict topology, hwloc_obj_type_t type,
				       unsigned nbobjs, unsigned *indexes, float *distances)
{
  unsigned *_indexes;
  float *_distances;

  /* copy the input arrays and give them to the topology */
  _indexes = malloc(nbobjs*sizeof(unsigned));
  memcpy(_indexes, indexes, nbobjs*sizeof(unsigned));
  _distances = malloc(nbobjs*nbobjs*sizeof(float));
  memcpy(_distances, distances, nbobjs*nbobjs*sizeof(float));
  return hwloc_topology__set_distance_matrix(topology, type, nbobjs, _indexes, _distances);
}

/* convert distance indexes that were previously stored in the topology
 * into actual objects.
 */
void hwloc_convert_distances_indexes_into_objects(struct hwloc_topology *topology)
{
  hwloc_obj_type_t type;
  for(type = HWLOC_OBJ_SYSTEM; type < HWLOC_OBJ_TYPE_MAX; type++) {
    unsigned nbobjs = topology->os_distances[type].nbobjs;
    unsigned *indexes = topology->os_distances[type].indexes;
    unsigned i;
    if (indexes) {
      hwloc_obj_t *objs = calloc(nbobjs, sizeof(hwloc_obj_t));
      /* traverse the topology and look for the relevant objects */
      for(i=0; i<nbobjs; i++) {
	hwloc_obj_t obj = hwloc_find_obj_by_type_and_os_index(topology->levels[0][0], type, indexes[i]);
	if (!obj) {
	  fprintf(stderr, "Ignoring %s distances from environment variable, unknown OS index %u\n",
		  hwloc_obj_type_string(type), indexes[i]);
	  free(objs);
	  objs = NULL;
	  break;
	}
	objs[i] = obj;
      }
      /* objs was either filled or freed/NULLed */
      topology->os_distances[type].objs = objs;
    }
  }
}

static void
hwloc_setup_distances_from_os_matrix(struct hwloc_topology *topology,
				     unsigned nbobjs,
				     hwloc_obj_t *objs, float *osmatrix)
{
  unsigned i, j, li, lj, minl;
  float min = FLT_MAX, max = FLT_MIN;
  hwloc_obj_t root;
  float *matrix;
  hwloc_cpuset_t set;
  unsigned relative_depth;
  int idx;

  /* find the root */
  set = hwloc_bitmap_alloc();
  for(i=0; i<nbobjs; i++)
    hwloc_bitmap_or(set, set, objs[i]->cpuset);
  root = hwloc_get_obj_covering_cpuset(topology, set);
  assert(root);
  if (!hwloc_bitmap_isequal(set, root->cpuset)) {
    /* partial distance matrix not including all the children of a single object */
    /* TODO insert an intermediate object (group?) covering only these children ? */
    hwloc_bitmap_free(set);
    return;
  }
  hwloc_bitmap_free(set);
  relative_depth = objs[0]->depth - root->depth; /* this assume that we have distances between objects of the same level */

  /* get the logical index offset, it's the min of all logical indexes */
  minl = UINT_MAX;
  for(i=0; i<nbobjs; i++)
    if (minl > objs[i]->logical_index)
      minl = objs[i]->logical_index;

  /* compute/check min/max values */
  for(i=0; i<nbobjs; i++)
    for(j=0; j<nbobjs; j++) {
      float val = osmatrix[i*nbobjs+j];
      if (val < min)
	min = val;
      if (val > max)
	max = val;
    }
  if (!min) {
    /* Linux up to 2.6.36 reports ACPI SLIT distances, which should be memory latencies.
     * Except of SGI IP27 (SGI Origin 200/2000 with MIPS processors) where the distances
     * are the number of hops between routers.
     */
    hwloc_debug("%s", "minimal distance is 0, matrix does not seem to contain latencies, ignoring\n");
    return;
  }

  /* store the normalized latency matrix in the root object */
  idx = root->distances_count++;
  root->distances = realloc(root->distances, root->distances_count * sizeof(struct hwloc_distances_s *));
  root->distances[idx] = malloc(sizeof(struct hwloc_distances_s));
  root->distances[idx]->relative_depth = relative_depth;
  root->distances[idx]->nbobjs = nbobjs;
  root->distances[idx]->latency = matrix = malloc(nbobjs*nbobjs*sizeof(float));
  root->distances[idx]->latency_base = (float) min;
#define NORMALIZE_LATENCY(d) ((d)/(min))
  root->distances[idx]->latency_max = NORMALIZE_LATENCY(max);
  for(i=0; i<nbobjs; i++) {
    li = objs[i]->logical_index - minl;
    matrix[li*nbobjs+li] = NORMALIZE_LATENCY(osmatrix[i*nbobjs+i]);
    for(j=i+1; j<nbobjs; j++) {
      lj = objs[j]->logical_index - minl;
      matrix[li*nbobjs+lj] = NORMALIZE_LATENCY(osmatrix[i*nbobjs+j]);
      matrix[lj*nbobjs+li] = NORMALIZE_LATENCY(osmatrix[j*nbobjs+i]);
    }
  }
}

/* convert internal distances into logically-ordered distances
 * that can be exposed in the API
 */
void
hwloc_finalize_logical_distances(struct hwloc_topology *topology)
{
  unsigned nbobjs;
  hwloc_obj_type_t type;
  int depth;

  for (type = HWLOC_OBJ_SYSTEM; type < HWLOC_OBJ_TYPE_MAX; type++) {
    nbobjs = topology->os_distances[type].nbobjs;
    if (!nbobjs)
      continue;

    depth = hwloc_get_type_depth(topology, type);
    if (depth == HWLOC_TYPE_DEPTH_UNKNOWN || depth == HWLOC_TYPE_DEPTH_MULTIPLE)
      continue;

    if (topology->os_distances[type].objs) {
      /* if we have objs, we must have distances as well,
       * thanks to hwloc_convert_distances_indexes_into_objects()
       */
      assert(topology->os_distances[type].distances);

      hwloc_setup_distances_from_os_matrix(topology, nbobjs,
					   topology->os_distances[type].objs,
					   topology->os_distances[type].distances);
    }
  }
}

/* destroy a object distances structure */
void
hwloc_free_logical_distances(struct hwloc_distances_s * dist)
{
  free(dist->latency);
  free(dist);
}

/*
 * Place objects in groups if they are in a transitive graph of minimal distances.
 * Return how many groups were created, or 0 if some incomplete distance graphs were found.
 */
static unsigned
hwloc_setup_group_from_min_distance(unsigned nbobjs,
				    float *_distances,
				    unsigned *groupids)
{
  float (*distances)[nbobjs][nbobjs] = (float (*)[nbobjs][nbobjs])_distances;
  float min_distance = FLT_MAX;
  unsigned groupid = 1;
  unsigned i,j,k;

  memset(groupids, 0, nbobjs*sizeof(*groupids));

  /* find the minimal distance */
  for(i=0; i<nbobjs; i++)
    for(j=i+1; j<nbobjs; j++)
      if ((*distances)[i][j] < min_distance)
	min_distance = (*distances)[i][j];
  hwloc_debug("found minimal distance %f between objects\n", min_distance);

  if (min_distance == FLT_MAX)
    return 0;

  /* build groups of objects connected with this distance */
  for(i=0; i<nbobjs; i++) {
    unsigned size;
    int firstfound;

    /* if already grouped, skip */
    if (groupids[i])
      continue;

    /* start a new group */
    groupids[i] = groupid;
    size = 1;
    firstfound = i;

    while (firstfound != -1) {
      /* we added new objects to the group, the first one was firstfound.
       * rescan all connections from these new objects (starting at first found) to any other objects,
       * so as to find new objects minimally-connected by transivity.
       */
      int newfirstfound = -1;
      for(j=firstfound; j<nbobjs; j++)
	if (groupids[j] == groupid)
	  for(k=0; k<nbobjs; k++)
	    if (!groupids[k] && (*distances)[j][k] == min_distance) {
	      groupids[k] = groupid;
	      size++;
	      if (newfirstfound == -1)
		newfirstfound = k;
	      if (i == j)
		hwloc_debug("object %u is minimally connected to %u\n", k, i);
	      else
	        hwloc_debug("object %u is minimally connected to %u through %u\n", k, i, j);
	    }
      firstfound = newfirstfound;
    }

    if (size == 1) {
      /* cancel this group and start over */
      groupids[i] = 0;
      continue;
    }

    /* valid this group */
    groupid++;
    hwloc_debug("found transitive graph with %u objects with minimal distance %f\n",
	       size, min_distance);
  }

  /* return the last id, since it's also the number of used group ids */
  return groupid-1;
}

/*
 * Look at object physical distances to group them,
 * after having done some basic sanity checks.
 */
static void
hwloc__setup_groups_from_distances(struct hwloc_topology *topology,
				   unsigned nbobjs,
				   struct hwloc_obj **objs,
				   float *_distances,
				   int depth)
{
  float (*distances)[nbobjs][nbobjs] = (float (*)[nbobjs][nbobjs])_distances;
  unsigned groupids[nbobjs];
  unsigned nbgroups;
  unsigned i,j;

  hwloc_debug("trying to group %s objects into Group objects according to physical distances\n",
	     hwloc_obj_type_string(objs[0]->type));

  if (nbobjs <= 2)
    return;

  nbgroups = hwloc_setup_group_from_min_distance(nbobjs, _distances, groupids);
  if (!nbgroups)
    return;

  if (nbgroups == 1) {
    hwloc_debug("%s", "ignoring Group object with all objects\n");
    return;
  }

  /* For convenience, put these declarations inside a block.  Saves us
     from a bunch of mallocs, particularly with the 2D array. */
  {
      hwloc_obj_t groupobjs[nbgroups];
      unsigned groupsizes[nbgroups];
      float groupdistances[nbgroups][nbgroups];
      /* create new Group objects and record their size */
      memset(groupsizes, 0, sizeof(groupsizes));
      for(i=0; i<nbgroups; i++) {
          /* create the Group object */
          hwloc_obj_t group_obj;
          group_obj = hwloc_alloc_setup_object(HWLOC_OBJ_GROUP, -1);
          group_obj->cpuset = hwloc_bitmap_alloc();
          hwloc_bitmap_zero(group_obj->cpuset);
          group_obj->attr->group.depth = depth;
          for (j=0; j<nbobjs; j++)
              if (groupids[j] == i+1) {
                  hwloc_bitmap_or(group_obj->cpuset, group_obj->cpuset, objs[j]->cpuset);
                  groupsizes[i]++;
              }
          hwloc_debug_1arg_bitmap("adding Group object with %u objects and cpuset %s\n",
                                  groupsizes[i], group_obj->cpuset);
          hwloc_insert_object_by_cpuset(topology, group_obj);
          groupobjs[i] = group_obj;
      }

      /* factorize distances */
      memset(groupdistances, 0, sizeof(groupdistances));
      for(i=0; i<nbobjs; i++)
          for(j=0; j<nbobjs; j++)
              groupdistances[groupids[i]-1][groupids[j]-1] += (*distances)[i][j];
      for(i=0; i<nbgroups; i++)
          for(j=0; j<nbgroups; j++)
              groupdistances[i][j] /= groupsizes[i]*groupsizes[j];
#ifdef HWLOC_DEBUG
      hwloc_debug("%s", "generated new distance matrix between groups:\n");
      hwloc_debug("%s", "  index");
      for(j=0; j<nbgroups; j++)
	hwloc_debug(" % 5d", (int) j); /* print index because os_index is -1 fro Groups */
      hwloc_debug("%s", "\n");
      for(i=0; i<nbgroups; i++) {
	hwloc_debug("  % 5d", (int) i);
	for(j=0; j<nbgroups; j++)
	  hwloc_debug(" %2.3f", groupdistances[i][j]);
	hwloc_debug("%s", "\n");
      }
#endif

      hwloc__setup_groups_from_distances(topology, nbgroups, groupobjs, (float*) groupdistances, depth + 1);
  }
}

/*
 * Look at object physical distances to group them.
 */
static void
hwloc_setup_groups_from_distances(struct hwloc_topology *topology,
				  unsigned nbobjs,
				  struct hwloc_obj **objs,
				  float *_distances)
{
  float (*distances)[nbobjs][nbobjs] = (float (*)[nbobjs][nbobjs])_distances;
  unsigned i,j;

  if (getenv("HWLOC_IGNORE_DISTANCES"))
    return;

#ifdef HWLOC_DEBUG
  hwloc_debug("%s", "trying to group objects using distance matrix:\n");
  hwloc_debug("%s", "  index");
  for(j=0; j<nbobjs; j++)
    hwloc_debug(" % 5d", (int) objs[j]->os_index);
  hwloc_debug("%s", "\n");
  for(i=0; i<nbobjs; i++) {
    hwloc_debug("  % 5d", (int) objs[i]->os_index);
    for(j=0; j<nbobjs; j++)
      hwloc_debug(" %2.3f", (*distances)[i][j]);
    hwloc_debug("%s", "\n");
  }
#endif

  /* check that the matrix is ok */
  for(i=0; i<nbobjs; i++) {
    for(j=i+1; j<nbobjs; j++) {
      /* should be symmetric */
      if ((*distances)[i][j] != (*distances)[j][i]) {
	hwloc_debug("distance matrix asymmetric ([%u,%u]=%f != [%u,%u]=%f), aborting\n",
		   i, j, (*distances)[i][j], j, i, (*distances)[j][i]);
	return;
      }
      /* diagonal is smaller than everything else */
      if ((*distances)[i][j] <= (*distances)[i][i]) {
	hwloc_debug("distance to self not strictly minimal ([%u,%u]=%f <= [%u,%u]=%f), aborting\n",
		   i, j, (*distances)[i][j], i, i, (*distances)[i][i]);
	return;
      }
    }
  }

  hwloc__setup_groups_from_distances(topology, nbobjs, objs, _distances, 0);
}

void
hwloc_group_by_distances(struct hwloc_topology *topology)
{
  unsigned nbobjs;
  hwloc_obj_type_t type;

  for (type = HWLOC_OBJ_SYSTEM; type < HWLOC_OBJ_TYPE_MAX; type++) {
    nbobjs = topology->os_distances[type].nbobjs;
    if (!nbobjs)
      continue;

    if (topology->os_distances[type].objs) {
      /* if we have objs, we must have distances as well,
       * thanks to hwloc_convert_distances_indexes_into_objects()
       */
      assert(topology->os_distances[type].distances);
      hwloc_setup_groups_from_distances(topology, nbobjs,
					topology->os_distances[type].objs,
					topology->os_distances[type].distances);
    }
  }
}