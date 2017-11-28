#define _GNU_SOURCE

#include <scorep/SCOREP_SubstratePlugins.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <execinfo.h>
#include <unistd.h>
#include <dlfcn.h>
#include <assert.h>

#include <inttypes.h>

#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

/* Internal setting for max number of threads */
#define MAX_THREADS 1024
#define HASH_SIZE 1024
#define CLEANUP 20

#define VERBOSE 1

/* from xavier aguilar */
/* if last_enter == 0 -> event flow without last_enter and stride */
struct temporal_event_flow
{
  uint64_t previous_region;
  uint64_t follow_region;
  uint64_t first_enter;
  uint64_t last_enter;
  uint64_t stride;

  int previous_exit;
  int follow_exit;


  /* the first time it has been called */
  uint64_t first_total_index;

  /* the last time it has been called */
  uint64_t last_total_index;

  /* stack definitions */
  uint64_t previous_stack_x;
  uint64_t follow_stack_x;

  /* if multiple temporal_event_flow have the same
   * previous_region, follow_region, stride, (last_enter-first_enter), previous_exit, follow_exit, previous_stack_x, follow_stack_x
   * they can be merged by appending the first enter of the second temporal_event_flow here */
  uint64_t nr_additional_first_enters;
  uint64_t * additional_first_enters;

  /* metrics */
  uint64_t metric_min[16];
  uint64_t metric_max[16];
  uint64_t metric_tot[16];
};

struct prev_based_storage
{
  uint64_t visits;

  /* flow rules that could still be reached per thread */
  int32_t nr_open_eventflows;
  struct temporal_event_flow ** open_event_flows;

  /* flow rules that can not be reached anymore per thread */
  int32_t nr_closed_event_flows;
  struct temporal_event_flow ** closed_event_flows;

#ifdef ONLY_ENTER
  /* metrics */
  uint64_t metric_min[16];
  uint64_t metric_max[16];
  uint64_t metric_tot[16];
#endif

};

struct per_thread_region_info
{
  /*nr of visits of this region per thread */
  struct prev_based_storage ** prev_info;
  uint64_t * avail_prevs;
  uint64_t nr_avail_prevs;

  uint64_t total_visits;
};

/* an entry for a region */
struct region_entry{
  uint64_t regionHandle;
  char * name;
  struct region_entry * next;

  /* information per thread */
  struct per_thread_region_info per_thread[MAX_THREADS];
};


/* per thread information */
struct thread_info{
  /* id of this thread */
  int64_t id;

  /*last region entered/exited */
  int64_t last_region;

  /* number of regions entered and exited */
  int64_t num_regions;

  /* index of last successor */
  uint32_t last_successor;

  /* whether last call had been exit */
  int last_is_exit;

  /* current stack depth */
  int32_t depth;

  /* stack */
  uint64_t stack[128];

  /* (PAPI) metrics */
  uint64_t last_metrics[16];

  /* stack */
  uint64_t last_stack_x;

#ifdef ONLY_ENTER
  /* BUG! this should point to the next for metric updates */
  struct prev_based_storage * last_prev;
  uint64_t last_metrics_prev[16];
#endif
};

static const SCOREP_SubstratePluginCallbacks * functions;


/* hash set for regions */
struct region_entry * region_entries[HASH_SIZE];

/* list for threads */
struct thread_info thread_infos[MAX_THREADS];

static char * metrics[16];
static int num_metrics;


/* number of registered threads */
static int num_threads=0;

/* pthread locks and keys */
static pthread_key_t thread_key;

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;


/* get a region from the registered regions */
static inline struct region_entry * get_region(uint64_t regionHandle)
{
  uint32_t index = regionHandle%HASH_SIZE;
  struct region_entry * current_entry=region_entries[index];
//  printf("Stuff %p\n",region_entries[32]);
  while (current_entry != NULL)
  {
    if (current_entry->regionHandle == regionHandle)
      return current_entry;
    current_entry=current_entry->next;
  }
  return NULL;
}

/* register a region to the region hash set*/
static inline void add_region(uint64_t regionHandle, const char * name)
{
  uint32_t index = regionHandle%HASH_SIZE;
  struct region_entry * last_entry=NULL;
  struct region_entry * current_entry=region_entries[index];
  pthread_mutex_lock(&thread_lock);
  while (current_entry != NULL)
  {
    last_entry = current_entry;
    current_entry = current_entry->next;
  }
  /* not a single entry followed */
  if (last_entry == NULL)
  {
    region_entries[index] = calloc(1, sizeof(struct region_entry));
    current_entry = region_entries[index];
  }
  else
  {
    last_entry->next = calloc(1, sizeof(struct region_entry));
    current_entry = last_entry->next;
  }
  current_entry->regionHandle = regionHandle;
  if ( name != NULL )
    current_entry->name = strdup(name);
  pthread_mutex_unlock(&thread_lock);
}

/* create stack_x, set depth=stack_depth-1 if exit for follow region, otherwise set stack_depth, set follow_region=0 if exit for follow region, otherwise follow_region*/
static inline uint64_t get_stack_x(struct thread_info * thread_info,int depth)
{
  int i;
  uint64_t retval=0;
#if NO_ID
  return thread_info->stack[depth-1];
#else
  for (i=0;i<depth;i++)
  {
    retval=retval^thread_info->stack[i]<<(i%16);
  }
  return retval;
#endif
}


/* define a flow definition that can be reached as unreachable
 * this should be done when stride!=0 and current_count > flow.count+flow.stride
 * */
static inline void append_to_closed_flows(struct prev_based_storage * region_thread_info, int event_flow_index)
{
  /* move rule to closed rules */

  /* extend closed by 1 */
  region_thread_info->closed_event_flows=realloc(region_thread_info->closed_event_flows,(region_thread_info->nr_closed_event_flows+1)*sizeof(struct temporal_event_flow *));
  /* copy reference */
  region_thread_info->closed_event_flows[region_thread_info->nr_closed_event_flows]=region_thread_info->open_event_flows[event_flow_index];

  region_thread_info->nr_closed_event_flows++;

  /* remove from active rules */

  /*overwrite by following entries (if there are any) */
  memmove(&region_thread_info->open_event_flows[event_flow_index],
      &region_thread_info->open_event_flows[event_flow_index+1],(region_thread_info->nr_open_eventflows-event_flow_index-1)*sizeof(struct temporal_event_flow *));

  region_thread_info->nr_open_eventflows--;
}

/* trims the closed flows by folding them
 * if two closed flows have the same stride, the same difference between first_enter and last_enter, the same is_exit variables and the same stacks
 * the latte rof the two is appended to the former. the metrics are aggregated.
 * */
static inline void clean_up_closed_flows(struct prev_based_storage * region_thread_info)
{
  int i,j,k;
  /* fold */
  for (i=0;i<region_thread_info->nr_closed_event_flows;i++)
  {
    struct temporal_event_flow * current=region_thread_info->closed_event_flows[i];
    for (j=i+1;j<region_thread_info->nr_closed_event_flows;j++)
    {
      struct temporal_event_flow * current2=region_thread_info->closed_event_flows[j];
      if (
          (current2->follow_stack_x == current->follow_stack_x) && (current2->previous_stack_x == current->previous_stack_x) &&
          (current2->stride == current->stride) && (current2->last_enter-current2->first_enter == current->last_enter-current->first_enter)
         )
      {
        current->additional_first_enters=realloc(current->additional_first_enters,(current->nr_additional_first_enters+1)*sizeof(int64_t));
        current->additional_first_enters[current->nr_additional_first_enters]=current2->first_enter;
        for (k=0;k<num_metrics;k++)
        {
          current->metric_tot[k]+=current2->metric_tot[k];
          if (current2->metric_min[k]<current->metric_min[k])
            current->metric_min[k]=current2->metric_min[k];
          if (current2->metric_max[k]>current->metric_max[k])
            current->metric_max[k]=current2->metric_max[k];
        }
        current->nr_additional_first_enters++;

        if (current2->last_total_index > current->last_total_index)
          current->last_total_index = current2->last_total_index;

        if (current2->first_total_index < current->first_total_index)
          current->first_total_index = current2->first_total_index;

        /* remove current2 by overwriting with following entry */
        memmove(&region_thread_info->closed_event_flows[j],&region_thread_info->closed_event_flows[j+1],(region_thread_info->nr_closed_event_flows-1-j)*sizeof(struct temporal_event_flow *));

        region_thread_info->nr_closed_event_flows--;

        /* reduce j by one because now closed_event_flows[j] is the previous closed_event_flows[j+1] due to the memmove above */
        j--;
      }
    }
  }
}


/**
 * on each enter (and maybe exit), this is called. here we check whether the successor rule is already known.
 */
static void insert_successor_rule(struct thread_info * thread_info, struct region_entry * next_region, int is_exit, uint64_t * metrics)
{
  int32_t i,j;
  int mapped=0;
  int strides_existed;
  uint64_t visits=0;

  uint64_t stack_x=thread_info->last_stack_x;
  uint64_t stack_x_follow=get_stack_x(thread_info,thread_info->depth)+is_exit;
  thread_info->last_stack_x = stack_x_follow;

  /* first enter -> only set last region */
  if (thread_info->num_regions==0)
  {
    thread_info->last_region=next_region->regionHandle;
    return;
  }
  struct region_entry * region=get_region(thread_info->last_region);
  struct per_thread_region_info * per_thread_info = &(region->per_thread[thread_info->id]);


  struct prev_based_storage * prev_info=NULL;
  /* find follow info */
  for (i=0;i<per_thread_info->nr_avail_prevs;i++)
  {
    if ( stack_x == per_thread_info->avail_prevs[i])
    {
      prev_info = per_thread_info->prev_info[i];
      break;
    }
  }
  if (prev_info == NULL)
  {
    per_thread_info->avail_prevs=realloc(per_thread_info->avail_prevs,(per_thread_info->nr_avail_prevs+1)*sizeof(uint64_t));
    per_thread_info->avail_prevs[per_thread_info->nr_avail_prevs] = stack_x;
    per_thread_info->prev_info=realloc(per_thread_info->prev_info,(per_thread_info->nr_avail_prevs+1)*sizeof(struct prev_based_storage *));
    per_thread_info->prev_info[per_thread_info->nr_avail_prevs] = calloc(1,sizeof(struct prev_based_storage));
    prev_info=per_thread_info->prev_info[per_thread_info->nr_avail_prevs];
    per_thread_info->nr_avail_prevs++;
  }


#ifdef ONLY_ENTER
  thread_info->last_prev = prev_info;
#endif
  prev_info->visits++;
  visits=prev_info->visits;

  /* other enters: check whether we know the last region in event flow rules */
  for (i=prev_info->nr_open_eventflows-1;i>=0;i--)
  {
    struct temporal_event_flow * current=prev_info->open_event_flows[i];
      /* if it is the expected stack */
      if (current->follow_stack_x==stack_x_follow)
      {
          if (current->stride != 0 )
          {
            /* if it is the expected stride */
            if ( ( current->stride + current->last_enter ) == visits)
            {

              current->last_enter = visits;

              /* check stack */
              current->last_total_index = thread_info->num_regions;
              for (j=0;j<num_metrics;j++)
              {
                if ((metrics[j]-thread_info->last_metrics[j])<current->metric_min[j])
                  current->metric_min[j]=metrics[j]-thread_info->last_metrics[j];
                if ((metrics[j]-thread_info->last_metrics[j])>current->metric_max[j])
                  current->metric_max[j]=metrics[j]-thread_info->last_metrics[j];
                current->metric_tot[j]+= metrics[j]-thread_info->last_metrics[j];
              }
              mapped=1;
              break;
            }
          }
          /* if we can set a stride */
          else
          {
            current->last_enter = visits;
            current->stride = current->last_enter - current->first_enter;
            current->last_total_index = thread_info->num_regions;
            for (j=0;j<num_metrics;j++)
            {
              if ((metrics[j]-thread_info->last_metrics[j])<current->metric_min[j])
                current->metric_min[j]=metrics[j]-thread_info->last_metrics[j];
              if ((metrics[j]-thread_info->last_metrics[j])>current->metric_max[j])
                current->metric_max[j]=metrics[j]-thread_info->last_metrics[j];
              current->metric_tot[j]+= metrics[j]-thread_info->last_metrics[j];
            }
            mapped=1;
            break;
          }
        }
  }

  /* try to close event flows that can not be used any more */
  if ((prev_info->visits%CLEANUP)==(CLEANUP-1))
    {
      for (i=0;i<prev_info->nr_open_eventflows;i++)
      {
        struct temporal_event_flow * current=prev_info->open_event_flows[i];
        if (current->stride > 0)
        {
          /* if this rule can not be reached any more */
          if (prev_info->visits > (current->last_enter+current->stride))
          {
            append_to_closed_flows(prev_info,i);
          }
        }
      }
      if ((prev_info->visits%(10*CLEANUP))==(CLEANUP-1))
      {
        clean_up_closed_flows(prev_info);
      }
    }
  /* if step could not be mapped to existing rule -> generate new rule */
  if (!mapped)
  {
    prev_info->open_event_flows = realloc(prev_info->open_event_flows,(prev_info->nr_open_eventflows+1)*sizeof(struct temporal_event_flow *));
    prev_info->open_event_flows[prev_info->nr_open_eventflows] = calloc(1,sizeof (struct temporal_event_flow));

    prev_info->open_event_flows[prev_info->nr_open_eventflows]->previous_region = thread_info->last_region;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->follow_region = next_region->regionHandle;

    prev_info->open_event_flows[prev_info->nr_open_eventflows]->first_enter = visits;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->last_enter = visits;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->last_total_index = thread_info->num_regions;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->first_total_index = thread_info->num_regions;

    prev_info->open_event_flows[prev_info->nr_open_eventflows]->follow_exit = is_exit;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->previous_exit = thread_info->last_is_exit;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->previous_stack_x = stack_x;
    prev_info->open_event_flows[prev_info->nr_open_eventflows]->follow_stack_x = stack_x_follow;

    for (j=0;j<num_metrics;j++)
    {
      prev_info->open_event_flows[prev_info->nr_open_eventflows]->metric_min[j] = metrics[j]-thread_info->last_metrics[j];
      prev_info->open_event_flows[prev_info->nr_open_eventflows]->metric_max[j] = metrics[j]-thread_info->last_metrics[j];
      prev_info->open_event_flows[prev_info->nr_open_eventflows]->metric_tot[j] = metrics[j]-thread_info->last_metrics[j];
    }
    prev_info->nr_open_eventflows++;

  }

  per_thread_info->total_visits++;

  thread_info->last_region = next_region->regionHandle;
  thread_info->last_is_exit = is_exit;
}

/* register a thread */
static inline struct thread_info * register_thread()
{
  unsigned long long ret;
  /* lock thread counter */
  pthread_mutex_lock(&thread_lock);
  /* get thread id */
  ret = num_threads;
  num_threads++;
  /* unlock thread counter */
  pthread_mutex_unlock(&thread_lock);
  /* store own thread info */
  pthread_setspecific(thread_key,&thread_infos[ret]);
  memset(&thread_infos[ret],0,sizeof(struct thread_info));
  thread_infos[ret].id=ret;

#ifdef CREATE_DYN_ID
  thread_infos[ret].depth=1;
#endif
  return &thread_infos[ret];
}

/* get the thread id of the current thread */
static inline struct thread_info * get_thread_info()
{
  struct thread_info * thread_i = pthread_getspecific(thread_key);
  if ( thread_i == NULL)
  {
    thread_i = register_thread();
  }
  return thread_i;
}

/* called when the program is started */
static int graphviz_init()
{
    unsigned int storage_needed, number;
    int i;
    char buffer[1024];
    char * binary_name;
    int binary_name_length;

    memset(region_entries,0,sizeof(struct region_entry *)*HASH_SIZE);
    memset(thread_infos,0,sizeof(struct thread_info *)*MAX_THREADS);
    pthread_key_create(&thread_key, NULL);

    /* register the main thread */
    register_thread();
		return 0;
}



/* get an ID of the current stack that is static for a single run (fast) */
static inline unsigned long long get_dynamic_id()
{
  unsigned long long id=0,i;
  int max_size=128;
  void * ips[128];
  int size;
  size= backtrace(ips,max_size);
  for (i=2;i<size;i++)
  {
    id ^=(unsigned long long)ips[i];
  }
  return id;
}


/* when entering a region, check whether it is known
 * (in hashset), then insert event flow
 */
static void graphviz_enter(
    struct SCOREP_Location* location,
    uint64_t                timestamp,
    SCOREP_RegionHandle     regionHandle,
    uint64_t*               metricValues)
{
  char * name=NULL;
  struct region_entry * future_region=get_region(regionHandle);
#ifdef ONLY_ENTER
  int j;
#endif
  if (future_region != NULL)
  {
    struct thread_info * ti = get_thread_info();

#ifdef CREATE_DYN_ID
    ti->stack[0] = get_dynamic_id();
#else
    ti->stack[ti->depth] = regionHandle;
    ti->depth++;
#endif
    insert_successor_rule( ti , future_region, 0 ,metricValues);
    ti->num_regions++;

#ifdef ONLY_ENTER
    if (ti->last_prev != NULL)
      for (j=0;j<num_metrics;j++)
      {
        if ( ti->last_prev->metric_min[j] > ti->last_metrics_prev[j])
          ti->last_prev->metric_min[j] = ti->last_metrics_prev[j];
        if ( ti->last_prev->metric_max[j] < ti->last_metrics_prev[j])
          ti->last_prev->metric_max[j] = ti->last_metrics_prev[j];
        ti->last_prev->metric_tot[j] += ti->last_metrics_prev[j];
      }
#endif
    memcpy(ti->last_metrics,metricValues,num_metrics*sizeof(uint64_t));
  }
}


/* when entering a region, check whether it is known
 * by libadapt (in hashset), then call libadapt enter
 */
static void graphviz_exit(
    struct SCOREP_Location* location,
    uint64_t                timestamp,
    SCOREP_RegionHandle     regionHandle,
    uint64_t*               metricValues)
{
  struct region_entry * future_region=get_region(regionHandle);
#ifdef ONLY_ENTER
  int j;
#endif
  if (future_region != NULL)
  {
    struct thread_info * ti = get_thread_info();
#ifdef CREATE_DYN_ID
    ti->stack[0] = get_dynamic_id();
#else
    ti->depth--;
#endif

#ifdef ONLY_ENTER
    for (j=0;j<num_metrics;j++)
    {
      ti->last_metrics_prev[j] = metricValues[j] - ti->last_metrics[j];
    }
#else
    insert_successor_rule(ti,future_region,1,metricValues);
#endif
    ti->num_regions++;
    memcpy(ti->last_metrics,metricValues,num_metrics*sizeof(uint64_t));
  }
}

/**
 * register metric. should be PAPI or SCOREP_METRIC_MODE_ACCUMULATED_START
 */
static void graphviz_define_metric(
    SCOREP_MetricHandle metricHandle,
    const char* name,
    SCOREP_MetricMode mode,
		SCOREP_MetricSourceType sourceType
)
{
  if (num_metrics==16) return;
  if (mode != SCOREP_METRIC_MODE_ACCUMULATED_START || sourceType != SCOREP_METRIC_SOURCE_TYPE_PAPI)
  {
    fprintf(stderr, "Metric %s has unsupported mode or source type\n",name);
  }
  else
  {
    metrics[num_metrics]=strdup(name);
    num_metrics++;
  }
}

/* when Score-P starts, all regions are defined, if the
 * region is known to libadapt, add it to hashset */
static void graphviz_define_region(
    SCOREP_RegionHandle     regionId,
    const char*             regionName,
    const char*             regionCanonicalName,
    SCOREP_ParadigmType     paradigm,
    SCOREP_RegionType       regionType)
{
#ifdef ONLY_ENTER
  if (paradigm == SCOREP_PARADIGM_OPENMP) {
    if (regionType != SCOREP_REGION_PARALLEL)
      return;
  }
  else if (paradigm != SCOREP_PARADIGM_MPI)
    printf("Warning: The graphviz plugin has been compiled for pure OpenMP/MPI instrumentation. The provided results might be incorrect\n");
#endif
  if (strstr(regionName,"!$omp flush @") != NULL)
    return;
  add_region(regionId, regionName);
}

/**
 * define a new handle. if region or metric: register
 */
static void graphviz_new_definition_handle (SCOREP_AnyHandle handle, SCOREP_HandleType type)
{
	if (type == SCOREP_HANDLE_TYPE_REGION)
	{
		const char * name = functions->SCOREP_RegionHandle_GetName(handle);
		const char * cname = functions->SCOREP_RegionHandle_GetCanonicalName(handle);
		SCOREP_ParadigmType par=functions->SCOREP_RegionHandle_GetParadigmType(handle);
		SCOREP_RegionType type = functions->SCOREP_RegionHandle_GetType(handle);
		graphviz_define_region(handle,name,cname,par,type);
	}
	if (type == SCOREP_HANDLE_TYPE_METRIC)
	{
		const char* name=functions->SCOREP_MetricHandle_GetName(handle);
		SCOREP_MetricMode mode=functions->SCOREP_MetricHandle_GetMode(handle);
		SCOREP_MetricSourceType srct=functions->SCOREP_MetricHandle_GetSourceType(handle);
		graphviz_define_metric(handle,name,mode,srct);
	}
}
/**
 * used for qsort, compare two uint64_ts
 */
static int compare( const void* a, const void* b)
{
     uint64_t int_a = * ( (uint64_t*) a );
     uint64_t int_b = * ( (uint64_t*) b );

     if ( int_a == int_b ) return 0;
     else if ( int_a < int_b ) return -1;
     else return 1;
}

/**
 * Find a stride over a stride
 */
static int64_t find_uber_stride(struct temporal_event_flow * flow)
{
  int index;
  int diff=flow->additional_first_enters[0]-flow->first_enter;

  qsort( flow->additional_first_enters, flow->nr_additional_first_enters, sizeof(uint64_t), compare );
  for (index=0;index<flow->nr_additional_first_enters-1;index++)
  {
    if (diff != (flow->additional_first_enters[index+1]-flow->additional_first_enters[index]) )
    {
      return -1;
    }
  }
  return diff;
}

/**
 * Close plugin and write data
 */
static void graphviz_finalize(void)
{
	FILE * fd;
	char buffer[1024];
	char directory[1024];
	uint32_t thread_num;
	int i, ret;
	uint32_t successor_rule_num;

	sprintf(directory, "%s/graphviz", functions->SCOREP_GetExperimentDirName());
	ret = mkdir(directory, S_IWUSR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP);
	if ((ret != 0))
	{
		DIR * dir = opendir(directory);
		if (dir == NULL)
		{
			fprintf(stderr,
					"Could not create/access directory \"%s\" (Error %d), using current working directory\n",
					directory, ret);
			sprintf(directory, "./");
		}
		else
		{
			closedir(dir);
		}
	}

	for (thread_num = 0; thread_num < num_threads; thread_num++)
	{
		uint64_t max_region_end = 0;
		uint64_t min_region_start = 1000;
		struct temporal_event_flow * last_trans_index = 0;
		struct temporal_event_flow * first_trans_index = 0;

		/* close all open event flows of this thread */
		for (i = 0; i < HASH_SIZE; i++)
		{
			struct region_entry * current_region = region_entries[i];

			while (current_region != NULL)
			{

				struct per_thread_region_info * current_region_thread_info =
						&current_region->per_thread[thread_num];
				if (current_region_thread_info != NULL)
				{
					for (uint32_t cur_follower_index = 0;
							cur_follower_index
							< current_region_thread_info->nr_avail_prevs;
							cur_follower_index++)
					{
						while (current_region_thread_info->prev_info[cur_follower_index]->nr_open_eventflows
								!= 0)
							append_to_closed_flows(
									current_region_thread_info->prev_info[cur_follower_index],
									0);
						clean_up_closed_flows(
								current_region_thread_info->prev_info[cur_follower_index]);

					}
				}
				current_region = current_region->next;
			}
		}

		/*
		 * print  dot
		 */

		sprintf(buffer, "%s/score-p-%d-%d.dot", directory, getpid(), thread_num);
		fd = fopen(buffer, "w+");
		if (fd == NULL)
		{
			fprintf(stderr,
					"Could not create file \"%s\" (Error: %d). Cannot write output.\n",
					buffer, errno);
			return;
		}
		fprintf(fd, "#!dot\n");
		fprintf(fd, "digraph {\n");

		fprintf(fd, "start [shape=Mdiamond];\n");

		/* TODO count nr nodes, total time to scale edges/nodes */

		for (i = 0; i < HASH_SIZE; i++)
		{
			struct region_entry * current_region = region_entries[i];
			while (current_region != NULL)
			{
				struct per_thread_region_info * current_region_thread_info =
						&current_region->per_thread[thread_num];
				for (uint32_t cur_follower_index = 0;
						cur_follower_index < current_region_thread_info->nr_avail_prevs;
						cur_follower_index++)
				{
					struct prev_based_storage * prev =
							current_region_thread_info->prev_info[cur_follower_index];
					for (successor_rule_num = 0;
							successor_rule_num < prev->nr_closed_event_flows;
							successor_rule_num++)
					{
						struct temporal_event_flow * current =
								prev->closed_event_flows[successor_rule_num];

						if (successor_rule_num == 0)
						{

							fprintf(fd,"box%"PRIu64"%"PRIu64"%s [ label=\"%s%s\"",
									current->previous_stack_x,
									current_region->regionHandle,
									current->previous_exit?"Exit":"",
											// if the previous had been exit
											current->previous_exit?"Exit ":"",
													current_region->name);

#ifdef ONLY_ENTER
							if (prev->metric_tot[1]!=0)
							{
								/* stall cycles per cycle */
								if ((double)prev->metric_tot[2]/(double)prev->metric_tot[1] < 0.5)
								{
									/* green to yellow*/
									fprintf(fd," color=\"#%.2xff00\"",(int)((double)prev->metric_tot[2]/(double)prev->metric_tot[1]*2.0*256.0));
								}
								else
								{
									/* yellow to red */
									fprintf(fd," color=\"#ff%.2x00\"",(int)(((double)prev->metric_tot[2]/(double)prev->metric_tot[1]-0.5)*2.0*256.0));
								}
							}
#endif
							fprintf(fd, "]\n");
						}

						if (current->nr_additional_first_enters == 0)
						{

							if (current->stride == 0)
							{
								fprintf(fd,"box%"PRIu64"%"PRIu64"%s -> box%"PRIu64"%"PRIu64"%s [ label=\"  %"PRIu64"  \"",
										current->previous_stack_x,
										current_region->regionHandle,
										/* if the previous had been exit */
										current->previous_exit?"Exit":"",
												/* if the current is exit */
												current->follow_stack_x,
												current->follow_region,
												current->follow_exit?"Exit ":"",
														current->first_enter);
							}
							else
							{
								fprintf(fd,"box%"PRIu64"%"PRIu64"%s -> box%"PRIu64"%"PRIu64"%s [ label=\"  %"PRIu64",%"PRIu64",%"PRIu64"  \"",
										current->previous_stack_x,
										current_region->regionHandle,
										/* if the previous had been exit */
										current->previous_exit?"Exit":"",
												/* if the current is exit */
												current->follow_stack_x,
												current->follow_region,
												current->follow_exit?"Exit ":"",
														current->first_enter,
														current->last_enter,
														current->stride);
							}
						}
						else
						{
							/* only one definition -> multiple with same stride */
							/* find a uber-stride that fits all */
							int uber_stride = find_uber_stride(current);
							if (uber_stride > 0)
							{
								/* print only a colored edge without label */
								fprintf(fd,"box%"PRIu64"%"PRIu64"%s -> box%"PRIu64"%"PRIu64"%s [ label=\"  %"PRIu64",%"PRIu64",%"PRIu64",%d,%"PRIu64"  \"",
										current->previous_stack_x,
										current_region->regionHandle,
										/* if the previous had been exit */
										current->previous_exit?"Exit":"",
												/* if the current is exit */
												current->follow_stack_x,
												current->follow_region,
												current->follow_exit?"Exit":"",
														current->first_enter,
														current->last_enter,
														current->stride,
														uber_stride,
														current->nr_additional_first_enters);
							}
							else
							{
								fprintf(fd,"box%"PRIu64"%"PRIu64"%s -> box%"PRIu64"%"PRIu64"%s [label=\"%"PRIu64" x (%"PRIu64", %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\"",
										current->previous_stack_x,
										current_region->regionHandle,
										/* if the previous had been exit */
										current->previous_exit?"Exit":"",
												/* if the current is exit */
												current->follow_stack_x,
												current->follow_region,
												current->follow_exit?"Exit":"",
														current->nr_additional_first_enters+1,
														current->stride,
														current->first_enter,
														current->additional_first_enters[0],
														current->additional_first_enters[1],
														current->additional_first_enters[2],
														current->additional_first_enters[3]);
							}
						}

						if (current->metric_tot[1] != 0)
						{
							/* stall cycles per cycle */
							if ((double) current->metric_tot[2] / (double) current->metric_tot[1] < 0.5)
								/* green to yellow*/
								fprintf(fd, " color=\"#%.2xff00\"",
										(int) ((double) current->metric_tot[2]
																			/ (double) current->metric_tot[1] * 2.0 * 256.0));
							else
							{
								/* yellow to red */
								fprintf(fd, " color=\"#ff%.2x00\"",
										(int) (((double) current->metric_tot[2]
																			 / (double) current->metric_tot[1] - 0.5) * 2.0 * 256.0));
							}
						}

						fprintf(fd, "];\n");
						/* mark to find last region in program flow*/
						if (max_region_end < (current->last_total_index + 1))
						{
							max_region_end = (current->last_total_index + 1);
							last_trans_index = current;
						}
						if (current->first_total_index < min_region_start)
						{
							min_region_start = current->first_total_index;
							first_trans_index = current;
						}

					}
				}
				current_region = current_region->next;
			}
		}

		if (last_trans_index == 0)
			return;

		/* check whether last element has been used */
		int used = 0;
		for (i = 0; i < HASH_SIZE; i++)
		{
			struct region_entry * current_region = region_entries[i];
			while (current_region != NULL)
			{
				struct per_thread_region_info * current_region_thread_info =
						&current_region->per_thread[thread_num];
				for (uint32_t cur_follower_index = 0;
						cur_follower_index < current_region_thread_info->nr_avail_prevs;
						cur_follower_index++)
				{
					struct prev_based_storage * prev =
							current_region_thread_info->prev_info[cur_follower_index];
					successor_rule_num = 0;
					struct temporal_event_flow * current =
							prev->closed_event_flows[successor_rule_num];
					if (current->previous_stack_x == last_trans_index->follow_stack_x
							&& current->previous_region == last_trans_index->follow_region
							&& current->previous_exit == last_trans_index->follow_exit)
					{
						used = 1;
					}
				}
				current_region = current_region->next;
			}
		}
		if (!used)
		{
			fprintf(fd,"box%"PRIu64"%"PRIu64"%s [ label=\"%s%s\"",
					last_trans_index->follow_stack_x,
					last_trans_index->follow_region,
					last_trans_index->follow_exit?"Exit":"",
							/* if the previous had been exit */
							last_trans_index->follow_exit?"Exit ":"",
									get_region(last_trans_index->follow_region)->name);
			fprintf(fd, "]\n");
		}

		fprintf(fd, "end [shape=Msquare];\n");
		/* print start to first entry */
		fprintf(fd,"\"start\" -> box%"PRIu64"%"PRIu64"%s\n",first_trans_index->previous_stack_x,first_trans_index->previous_region,first_trans_index->previous_exit?"Exit":"");
		fprintf(fd,"box%"PRIu64"%"PRIu64"%s -> \"end\"\n",last_trans_index->follow_stack_x,last_trans_index->follow_region,last_trans_index->follow_exit?"Exit":"");
		/* print end */
		fprintf(fd, "}\n");
		fclose(fd);

	}
}

static size_t substrate_id;

/**
 * Gets the internal Score-P id for this plugin.
 */
static void graphviz_assign_id( size_t s_id )
{
	substrate_id = s_id;
}


/**
 * Get callbacks from Score-P
 * TODO check size
 */
static void graphviz_set_callbacks(
		const SCOREP_SubstratePluginCallbacks * passed, __attribute__((unused)) size_t size)
{
	functions = passed;
}


/* we need the output folder, therefore we tell Score-P about it */
static int64_t graphviz_get_requirement( SCOREP_Substrates_RequirementFlag flag )
{
  switch ( flag )
  {
      case SCOREP_SUBSTRATES_REQUIREMENT_EXPERIMENT_DIRECTORY:
          return 1;
      default:
          return 0;
  }
}

/**
 * Define event functions
 */
uint32_t graphviz_event_callbacks(__attribute__((unused)) SCOREP_Substrates_Mode mode,
		SCOREP_Substrates_Callback** functions)
{
	SCOREP_Substrates_Callback* ret_fun = calloc(SCOREP_SUBSTRATES_NUM_EVENTS,
			sizeof(SCOREP_Substrates_Callback));
	ret_fun[SCOREP_EVENT_ENTER_REGION] =
			(SCOREP_Substrates_Callback) graphviz_enter;
	ret_fun[SCOREP_EVENT_EXIT_REGION] = (SCOREP_Substrates_Callback) graphviz_exit;
	*functions = ret_fun;
	return SCOREP_SUBSTRATES_NUM_EVENTS;
}

/**
 * Define management functions and name from plugin
 */
SCOREP_SUBSTRATE_PLUGIN_ENTRY( graphviz)
{
	SCOREP_SubstratePluginInfo info;
	memset(&info, 0, sizeof(SCOREP_SubstratePluginInfo));
	info.init                  = graphviz_init;
    info.assign_id             = graphviz_assign_id;
	info.write_data            = graphviz_finalize;
	info.new_definition_handle = graphviz_new_definition_handle;
	info.get_event_functions   = graphviz_event_callbacks;
	info.set_callbacks         = graphviz_set_callbacks;
    info.get_requirement       = graphviz_get_requirement;
	return info;
}


