/**
 * collectd - src/statsd.c
 * Copyright (C) 2013       Florian octo Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 */

#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include "configfile.h"
#include "utils_avltree.h"
#include "utils_complain.h"
#include "utils_latency.h"

#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

/* AIX doesn't have MSG_DONTWAIT */
#ifndef MSG_DONTWAIT
#  define MSG_DONTWAIT MSG_NONBLOCK
#endif

#ifndef STATSD_DEFAULT_HOST
# define STATSD_DEFAULT_HOST "localhost"
#endif

#ifndef STATSD_DEFAULT_NODE_NAME
# define STATSD_DEFAULT_NODE_NAME "default"
#endif

#ifndef STATSD_DEFAULT_SERVICE
# define STATSD_DEFAULT_SERVICE "8125"
#endif

enum metric_type_e
{
  STATSD_COUNTER,
  STATSD_TIMER,
  STATSD_GAUGE,
  STATSD_SET
};
typedef enum metric_type_e metric_type_t;

struct statsd_metric_s
{
  metric_type_t type;
  double value;
  latency_counter_t *latency;
  c_avl_tree_t *set;
  unsigned long updates_num;
};
typedef struct statsd_metric_s statsd_metric_t;

struct statsd_config_s {
  char *node_name;
  char *host;
  char *service;

  c_avl_tree_t   *metrics_tree;
  pthread_mutex_t metrics_lock;

  _Bool delete_counters;
  _Bool delete_timers;
  _Bool delete_gauges;
  _Bool delete_sets;

  double *timer_percentile;
  size_t  timer_percentile_num;

  _Bool timer_lower;
  _Bool timer_upper;
  _Bool timer_sum;
  _Bool timer_count;

  /* metrics name's operatons
     graphite:
     legacyNamespace:  use the legacy namespace [default: true]
     globalPrefix:     global prefix to use for sending stats to graphite [default: "stats"]
     prefixCounter:    graphite prefix for counter metrics [default: "counters"]
     prefixTimer:      graphite prefix for timer metrics [default: "timers"]
     prefixGauge:      graphite prefix for gauge metrics [default: "gauges"]
     prefixSet:        graphite prefix for set metrics [default: "sets"]

     (c) by statsd nodejs
  */

  _Bool leave_metrics_name_asis;

  char *global_prefix;
  char *counter_prefix;
  char *timer_prefix;
  char *gauge_prefix;
  char *set_prefix;

  char *global_postfix;
};
typedef struct statsd_config_s statsd_config_t;

struct statsd_thread_s {
  pthread_t thr;
  statsd_config_t *conf;
};
typedef struct statsd_thread_s statsd_thread_t;

static statsd_thread_t *statsd_threads     = NULL;
static size_t           statsd_threads_num = 0;

struct fds_poll_s {
  struct pollfd *fds;
  size_t fds_num;
};
typedef struct fds_poll_s fds_poll_t;

/* Must hold metrics_lock when calling this function. */
static statsd_metric_t *statsd_metric_lookup_unsafe (statsd_config_t *conf, char const *name, /* {{{ */
    metric_type_t type)
{
  char key[DATA_MAX_NAME_LEN + 2];
  char *key_copy;
  statsd_metric_t *metric;
  int status;
  c_avl_tree_t *metrics_tree = conf->metrics_tree;

  switch (type)
  {
    case STATSD_COUNTER: key[0] = 'c'; break;
    case STATSD_TIMER:   key[0] = 't'; break;
    case STATSD_GAUGE:   key[0] = 'g'; break;
    case STATSD_SET:     key[0] = 's'; break;
    default: return (NULL);
  }

  key[1] = ':';
  sstrncpy (&key[2], name, sizeof (key) - 2);

  status = c_avl_get (metrics_tree, key, (void *) &metric);
  if (status == 0)
    return (metric);

  key_copy = strdup (key);
  if (key_copy == NULL)
  {
    ERROR ("statsd plugin: strdup failed.");
    return (NULL);
  }

  metric = malloc (sizeof (*metric));
  if (metric == NULL)
  {
    ERROR ("statsd plugin: malloc failed.");
    sfree (key_copy);
    return (NULL);
  }
  memset (metric, 0, sizeof (*metric));

  metric->type = type;
  metric->latency = NULL;
  metric->set = NULL;

  status = c_avl_insert (metrics_tree, key_copy, metric);
  if (status != 0)
  {
    ERROR ("statsd plugin: c_avl_insert failed.");
    sfree (key_copy);
    sfree (metric);
    return (NULL);
  }

  return (metric);
} /* }}} statsd_metric_lookup_unsafe */

static int statsd_metric_set (statsd_config_t *conf,char const *name, double value, /* {{{ */
    metric_type_t type)
{
  statsd_metric_t *metric;

  pthread_mutex_lock (&conf->metrics_lock);

  metric = statsd_metric_lookup_unsafe (conf, name, type);
  if (metric == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    return (-1);
  }

  metric->value = value;
  metric->updates_num++;

  pthread_mutex_unlock (&conf->metrics_lock);

  return (0);
} /* }}} int statsd_metric_set */

static int statsd_metric_add (statsd_config_t *conf, char const *name, double delta, /* {{{ */
    metric_type_t type)
{
  statsd_metric_t *metric;

  pthread_mutex_lock (&conf->metrics_lock);

  metric = statsd_metric_lookup_unsafe (conf, name, type);
  if (metric == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    return (-1);
  }

  metric->value += delta;
  metric->updates_num++;

  pthread_mutex_unlock (&conf->metrics_lock);

  return (0);
} /* }}} int statsd_metric_add */

static void statsd_metric_free (statsd_metric_t *metric) /* {{{ */
{
  if (metric == NULL)
    return;

  if (metric->latency != NULL)
  {
    latency_counter_destroy (metric->latency);
    metric->latency = NULL;
  }

  if (metric->set != NULL)
  {
    void *key;
    void *value;

    while (c_avl_pick (metric->set, &key, &value) == 0)
    {
      sfree (key);
      assert (value == NULL);
    }

    c_avl_destroy (metric->set);
    metric->set = NULL;
  }

  sfree (metric);
} /* }}} void statsd_metric_free */

static int statsd_parse_value (char const *str, value_t *ret_value) /* {{{ */
{
  char *endptr = NULL;

  ret_value->gauge = (gauge_t) strtod (str, &endptr);
  if ((str == endptr) || ((endptr != NULL) && (*endptr != 0)))
    return (-1);

  return (0);
} /* }}} int statsd_parse_value */

static int statsd_handle_counter (statsd_config_t *conf, char const *name, /* {{{ */
    char const *value_str,
    char const *extra)
{
  value_t value;
  value_t scale;
  int status;

  if ((extra != NULL) && (extra[0] != '@'))
    return (-1);

  scale.gauge = 1.0;
  if (extra != NULL)
  {
    status = statsd_parse_value (extra + 1, &scale);
    if (status != 0)
      return (status);

    if (!isfinite (scale.gauge) || (scale.gauge <= 0.0) || (scale.gauge > 1.0))
      return (-1);
  }

  value.gauge = 1.0;
  status = statsd_parse_value (value_str, &value);
  if (status != 0)
    return (status);

  return (statsd_metric_add (conf, name, (double) (value.gauge / scale.gauge),
        STATSD_COUNTER));
} /* }}} int statsd_handle_counter */

static int statsd_handle_gauge (statsd_config_t *conf, char const *name, /* {{{ */
    char const *value_str)
{
  value_t value;
  int status;

  value.gauge = 0;
  status = statsd_parse_value (value_str, &value);
  if (status != 0)
    return (status);

  if ((value_str[0] == '+') || (value_str[0] == '-'))
    return (statsd_metric_add (conf, name, (double) value.gauge, STATSD_GAUGE));
  else
    return (statsd_metric_set (conf, name, (double) value.gauge, STATSD_GAUGE));
} /* }}} int statsd_handle_gauge */

static int statsd_handle_timer (statsd_config_t *conf, char const *name, /* {{{ */
    char const *value_str,
    char const *extra)
{
  statsd_metric_t *metric;
  value_t value_ms;
  value_t scale;
  cdtime_t value;
  int status;

  if ((extra != NULL) && (extra[0] != '@'))
    return (-1);

  scale.gauge = 1.0;
  if (extra != NULL)
  {
    status = statsd_parse_value (extra + 1, &scale);
    if (status != 0)
      return (status);

    if (!isfinite (scale.gauge) || (scale.gauge <= 0.0) || (scale.gauge > 1.0))
      return (-1);
  }

  value_ms.derive = 0;
  status = statsd_parse_value (value_str, &value_ms);
  if (status != 0)
    return (status);

  value = DOUBLE_TO_CDTIME_T (value_ms.gauge / scale.gauge);

  pthread_mutex_lock (&conf->metrics_lock);

  metric = statsd_metric_lookup_unsafe (conf, name, STATSD_TIMER);
  if (metric == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    return (-1);
  }

  if (metric->latency == NULL)
    metric->latency = latency_counter_create ();
  if (metric->latency == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    return (-1);
  }

  latency_counter_add (metric->latency, value);
  metric->updates_num++;

  pthread_mutex_unlock (&conf->metrics_lock);
  return (0);
} /* }}} int statsd_handle_timer */

static int statsd_handle_set (statsd_config_t *conf, char const *name, /* {{{ */
    char const *set_key_orig)
{
  statsd_metric_t *metric = NULL;
  char *set_key;
  int status;


  pthread_mutex_lock (&conf->metrics_lock);

  metric = statsd_metric_lookup_unsafe (conf, name, STATSD_SET);
  if (metric == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    return (-1);
  }

  /* Make sure metric->set exists. */
  if (metric->set == NULL)
    metric->set = c_avl_create ((void *) strcmp);

  if (metric->set == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    ERROR ("statsd plugin: c_avl_create failed.");
    return (-1);
  }

  set_key = strdup (set_key_orig);
  if (set_key == NULL)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    ERROR ("statsd plugin: strdup failed.");
    return (-1);
  }

  status = c_avl_insert (metric->set, set_key, /* value = */ NULL);
  if (status < 0)
  {
    pthread_mutex_unlock (&conf->metrics_lock);
    if (status < 0)
      ERROR ("statsd plugin: c_avl_insert (\"%s\") failed with status %i.",
          set_key, status);
    sfree (set_key);
    return (-1);
  }
  else if (status > 0) /* key already exists */
  {
    sfree (set_key);
  }

  metric->updates_num++;

  pthread_mutex_unlock (&conf->metrics_lock);
  return (0);
} /* }}} int statsd_handle_set */

static int statsd_parse_line (statsd_config_t *conf, char *buffer) /* {{{ */
{
  char *name = buffer;
  char *value;
  char *type;
  char *extra;

  type = strchr (name, '|');
  if (type == NULL)
    return (-1);
  *type = 0;
  type++;

  value = strrchr (name, ':');
  if (value == NULL)
    return (-1);
  *value = 0;
  value++;

  extra = strchr (type, '|');
  if (extra != NULL)
  {
    *extra = 0;
    extra++;
  }

  if (strcmp ("c", type) == 0)
    return (statsd_handle_counter (conf, name, value, extra));
  else if (strcmp ("ms", type) == 0)
    return (statsd_handle_timer (conf, name, value, extra));

  /* extra is only valid for counters and timers */
  if (extra != NULL)
    return (-1);

  if (strcmp ("g", type) == 0)
    return (statsd_handle_gauge (conf, name, value));
  else if (strcmp ("s", type) == 0)
    return (statsd_handle_set (conf, name, value));
  else
    return (-1);
} /* }}} void statsd_parse_line */

static void statsd_parse_buffer (statsd_config_t *conf, char *buffer) /* {{{ */
{
  DEBUG("statsd plugin: buffer '%s'", buffer);

  while (buffer != NULL)
  {
    char orig[64];
    char *next;
    int status;

    next = strchr (buffer, '\n');
    if (next != NULL)
    {
      *next = 0;
      next++;
    }

    if (*buffer == 0)
    {
      buffer = next;
      continue;
    }

    sstrncpy (orig, buffer, sizeof (orig));

    status = statsd_parse_line (conf, buffer);
    if (status != 0)
      ERROR ("statsd plugin: Unable to parse line: \"%s\"", orig);

    buffer = next;
  }
} /* }}} void statsd_parse_buffer */

static void statsd_network_read (statsd_config_t *conf, int fd) /* {{{ */
{
  char buffer[4096] = {0};
  size_t buffer_size;
  ssize_t status;

  status = recv (fd, buffer, sizeof (buffer), /* flags = */ MSG_DONTWAIT);
  if (status < 0)
  {
    char errbuf[1024];

    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return;

    ERROR ("statsd plugin: recv(2) failed: %s",
        sstrerror (errno, errbuf, sizeof (errbuf)));
    return;
  }

  buffer_size = (size_t) status;
  if (buffer_size >= sizeof (buffer))
    buffer_size = sizeof (buffer) - 1;
  buffer[buffer_size] = 0;

  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
  statsd_parse_buffer (conf, buffer);
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
} /* }}} void statsd_network_read */

static int statsd_network_init (statsd_config_t *conf, fds_poll_t *ret_fds) /* {{{ */
{
  struct pollfd *fds = NULL;
  size_t fds_num = 0;

  struct addrinfo ai_hints;
  struct addrinfo *ai_list = NULL;
  struct addrinfo *ai_ptr;
  int status;

  char const *host = (conf->host != NULL) ? conf->host : STATSD_DEFAULT_HOST;
  char const *service = (conf->service != NULL)
    ? conf->service : STATSD_DEFAULT_SERVICE;

  memset (&ai_hints, 0, sizeof (ai_hints));
  ai_hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
  ai_hints.ai_flags |= AI_ADDRCONFIG;
#endif
  ai_hints.ai_family = AF_UNSPEC;
  ai_hints.ai_socktype = SOCK_DGRAM;

  status = getaddrinfo (host, service, &ai_hints, &ai_list);

  if (status != 0)
  {
    ERROR ("statsd plugin: getaddrinfo (\"%s\", \"%s\") failed: %s",
        host, service, gai_strerror (status));
    return (status);
  }

  for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next)
  {
    int fd;
    struct pollfd *tmp;

    char dbg_host[NI_MAXHOST] = {0};
    char dbg_service[NI_MAXSERV] = {0};

    fd = socket (ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
    if (fd < 0)
    {
      char errbuf[1024];
      ERROR ("statsd plugin: socket(2) failed: %s",
          sstrerror (errno, errbuf, sizeof (errbuf)));
      continue;
    }

    getnameinfo (ai_ptr->ai_addr, ai_ptr->ai_addrlen,
        dbg_host, sizeof (dbg_host), dbg_service, sizeof (dbg_service),
        NI_DGRAM | NI_NUMERICHOST | NI_NUMERICSERV);
    DEBUG ("statsd plugin: Trying to bind to [%s]:%s ...", dbg_host, dbg_service);

    status = bind (fd, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
    if (status != 0)
    {
      char errbuf[1024];
      ERROR ("statsd plugin: bind(2) failed: %s",
          sstrerror (errno, errbuf, sizeof (errbuf)));
      close (fd);
      continue;
    }

    tmp = realloc (fds, sizeof (*fds) * (fds_num + 1));
    if (tmp == NULL)
    {
      ERROR ("statsd plugin: realloc failed.");
      close (fd);
      continue;
    }
    fds = tmp;
    tmp = fds + fds_num;
    fds_num++;

    memset (tmp, 0, sizeof (*tmp));
    tmp->fd = fd;
    tmp->events = POLLIN | POLLPRI;
  }

  freeaddrinfo (ai_list);

  if (fds_num == 0)
  {
    ERROR ("statsd plugin: Unable to create listening socket for [%s]:%s.",
        (host != NULL) ? host : "::", service);
    return (ENOENT);
  }

  ret_fds->fds = fds;
  ret_fds->fds_num = fds_num;
  return (0);
} /* }}} int statsd_network_init */

static statsd_config_t *statsd_config_alloc(void)
{
  statsd_config_t *conf = NULL;

  conf = malloc(sizeof(statsd_config_t));
  if (NULL == conf) {
      ERROR ("statsd plugin: malloc failed.");
      return NULL;
  }

  memset(conf, 0, sizeof(statsd_config_t));
  pthread_mutex_init (&conf->metrics_lock, /* attr = */ NULL);
  conf->metrics_tree = c_avl_create ((void *) strcmp);

  return conf;
}

static void statsd_config_free(statsd_config_t *conf)
{
  void *key = NULL;
  void *value = NULL;

  if (NULL == conf)
    return;

  while (c_avl_pick (conf->metrics_tree, &key, &value) == 0) {
    sfree (key);
    statsd_metric_free (value);
  }

  c_avl_destroy(conf->metrics_tree);

  sfree(conf->node_name);
  sfree(conf->host);
  sfree(conf->service);
  sfree(conf->timer_percentile);
  sfree(conf->global_prefix);
  sfree(conf->counter_prefix);
  sfree(conf->timer_prefix);
  sfree(conf->gauge_prefix);
  sfree(conf->set_prefix);
  sfree(conf->global_postfix);

  sfree(conf);
  conf = NULL;

  return;
}

static void statsd_threads_free(void)
{
  int thread_num = 0;

  for (thread_num = 0; thread_num < statsd_threads_num; thread_num++) {
    statsd_config_free(statsd_threads[thread_num].conf);
  }

  sfree(statsd_threads);
  statsd_threads = NULL;
  statsd_threads_num = 0;

  return;
}

static void statsd_network_release(void *args)
{
  fds_poll_t *fds = args;
  size_t i = 0;

  if (NULL == fds || NULL == fds->fds)
    return;

  /* Clean up */
  for (i = 0; i < fds->fds_num; i++)
    close (fds->fds[i].fd);

  sfree (fds->fds);

  return;
}

static void *statsd_network_thread (void *args) /* {{{ */
{
  statsd_config_t *conf = args;
  fds_poll_t fds;
  int status;
  size_t i;

  memset(&fds, 0, sizeof(fds_poll_t));

  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

  status = statsd_network_init (conf, &fds);
  if (status != 0)
  {
    ERROR ("statsd plugin: Unable to open listening sockets.");
    pthread_exit ((void *) 0);
  }

  pthread_cleanup_push(statsd_network_release, &fds);

  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

  while (1) {

    status = poll (fds.fds, (nfds_t) fds.fds_num, /* timeout = */ -1);

    if (status < 0) {
      char errbuf[1024] = {0};

      if (EAGAIN == errno)
        continue;

      if (EINTR == errno) {
        DEBUG("statsd plugin: poll(2) has been interrupted");
        break;
      }

      ERROR ("statsd plugin: poll(2) failed: %s",
             sstrerror (errno, errbuf, sizeof (errbuf)));
      break;
    }

    DEBUG("statsd plugin: ohh some moving in the sockets");

    for (i = 0; i < fds.fds_num; i++) {
      if ((fds.fds[i].revents & (POLLIN | POLLPRI)) == 0)
        continue;

      statsd_network_read(conf, fds.fds[i].fd);
      fds.fds[i].revents = 0;
    }
    pthread_testcancel();
  } /* wait for pthread_cancel */

  pthread_cleanup_pop(1);

  return ((void *) 0);
} /* }}} void *statsd_network_thread */

static int statsd_config_timer_percentile (statsd_config_t *conf, oconfig_item_t *ci) /* {{{ */
{
  double percent = NAN;
  double *tmp;
  int status;

  status = cf_util_get_double (ci, &percent);
  if (status != 0)
    return (status);

  if ((percent <= 0.0) || (percent >= 100))
  {
    ERROR ("statsd plugin: The value for \"%s\" must be between 0 and 100, "
        "exclusively.", ci->key);
    return (ERANGE);
  }

  tmp = realloc (conf->timer_percentile,
      sizeof (double) * (conf->timer_percentile_num + 1));
  if (tmp == NULL)
  {
    ERROR ("statsd plugin: realloc failed.");
    return (ENOMEM);
  }
  conf->timer_percentile = tmp;
  conf->timer_percentile[conf->timer_percentile_num] = percent;
  conf->timer_percentile_num++;

  return (0);
} /* }}} int statsd_config_timer_percentile */


static int statsd_config_node (statsd_config_t *conf, oconfig_item_t *ci)
{
  int i;

  for (i = 0; i < ci->children_num; i++) {

    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Host", child->key) == 0)
      cf_util_get_string (child, &conf->host);
    else if (strcasecmp ("Port", child->key) == 0)
      cf_util_get_service (child, &conf->service);
    else if (strcasecmp ("DeleteCounters", child->key) == 0)
      cf_util_get_boolean (child, &conf->delete_counters);
    else if (strcasecmp ("DeleteTimers", child->key) == 0)
      cf_util_get_boolean (child, &conf->delete_timers);
    else if (strcasecmp ("DeleteGauges", child->key) == 0)
      cf_util_get_boolean (child, &conf->delete_gauges);
    else if (strcasecmp ("DeleteSets", child->key) == 0)
      cf_util_get_boolean (child, &conf->delete_sets);
    else if (strcasecmp ("TimerLower", child->key) == 0)
      cf_util_get_boolean (child, &conf->timer_lower);
    else if (strcasecmp ("TimerUpper", child->key) == 0)
      cf_util_get_boolean (child, &conf->timer_upper);
    else if (strcasecmp ("TimerSum", child->key) == 0)
      cf_util_get_boolean (child, &conf->timer_sum);
    else if (strcasecmp ("TimerCount", child->key) == 0)
      cf_util_get_boolean (child, &conf->timer_count);
    else if (strcasecmp ("LeaveMetricsNameASIS", child->key) == 0)
      cf_util_get_boolean (child, &conf->leave_metrics_name_asis);
    else if (strcasecmp ("GlobalPrefix", child->key) == 0)
      cf_util_get_string (child, &conf->global_prefix);
    else if (strcasecmp ("CounterPrefix", child->key) == 0)
      cf_util_get_string (child, &conf->counter_prefix);
    else if (strcasecmp ("TimerPrefix", child->key) == 0)
      cf_util_get_string (child, &conf->timer_prefix);
    else if (strcasecmp ("GaugePrefix", child->key) == 0)
      cf_util_get_string (child, &conf->gauge_prefix);
    else if (strcasecmp ("SetPrefix", child->key) == 0)
      cf_util_get_string (child, &conf->set_prefix);
    else if (strcasecmp ("GlobalPostfix", child->key) == 0)
      cf_util_get_string (child, &conf->global_postfix);
    else if (strcasecmp ("TimerPercentile", child->key) == 0)
      statsd_config_timer_percentile (conf, child);
    else
      ERROR ("statsd plugin: The \"%s\" config option is not valid.",
             child->key);
  }

  return (0);
}

static int statsd_config (oconfig_item_t *ci) /* {{{ */
{
  int i;

  for (i = 0; i < ci->children_num; i++) {

    oconfig_item_t *child = ci->children + i;
    statsd_config_t *conf = NULL;
    statsd_thread_t *tmp = NULL;

    if (NULL == statsd_threads) {
      statsd_threads = malloc(sizeof(statsd_thread_t));

      if (NULL == statsd_threads) {
        ERROR ("statsd plugin: malloc failed.");
        return (ENOMEM);
      }

      memset(statsd_threads, 0, sizeof(statsd_thread_t));

    } else {
      tmp = realloc(statsd_threads, sizeof (statsd_config_t) * (statsd_threads_num + 1));

      if (NULL == tmp) {
        ERROR ("statsd plugin: realloc failed.");
        statsd_threads_free();
        return (ENOMEM);
      }

      statsd_threads = tmp;
      memset(statsd_threads + statsd_threads_num, 0, sizeof(statsd_config_t));
    }

    statsd_threads_num += 1;
    conf = statsd_config_alloc();

    if (NULL == conf) {
      ERROR ("statsd plugin: malloc failed.");
      statsd_threads_free();
      return (ENOMEM);
    }

    statsd_threads[statsd_threads_num - 1].conf = conf;

    if (strcasecmp ("Node", child->key) == 0) {
      cf_util_get_string (child, &conf->node_name);

      if (NULL == conf->node_name) {
        conf->node_name = strdup(STATSD_DEFAULT_NODE_NAME);
        if (NULL == conf->node_name) {
          ERROR ("statsd plugin: malloc failed.");
          statsd_threads_free();
          return (ENOMEM);
        }
      }

    } else
      ERROR ("statsd plugin: The \"%s\" config option is not valid.",
             child->key);

    statsd_config_node(conf, child);
  }

  return (0);
} /* }}} int statsd_config */

static int statsd_init (void) /* {{{ */
{
  int thread_num = 0;

  for (thread_num = 0; thread_num < statsd_threads_num; thread_num++) {
    int status = 0;

    status = plugin_thread_create (&statsd_threads[thread_num].thr,
                                   /* attr = */ NULL,
                                   statsd_network_thread,
                                   statsd_threads[thread_num].conf);

    if (status != 0) {
      char errbuf[1024] = {0};
      ERROR ("statsd plugin: pthread_create failed: %s",
             sstrerror (errno, errbuf, sizeof (errbuf)));
      return (status);
    }
  }
  return (0);
} /* }}} int statsd_init */

/* Must hold metrics_lock when calling this function. */
static int statsd_metric_clear_set_unsafe (statsd_metric_t *metric) /* {{{ */
{
  void *key;
  void *value;

  if ((metric == NULL) || (metric->type != STATSD_SET))
    return (EINVAL);

  if (metric->set == NULL)
    return (0);

  while (c_avl_pick (metric->set, &key, &value) == 0)
  {
    sfree (key);
    sfree (value);
  }

  return (0);
} /* }}} int statsd_metric_clear_set_unsafe */

/* Must hold metrics_lock when calling this function. */
static int statsd_metric_submit_unsafe (statsd_config_t *conf, char const *name, /* {{{ */
    statsd_metric_t const *metric)
{
  value_t values[1];
  value_list_t vl = VALUE_LIST_INIT;
  char *global_prefix = NULL;
  char *type_prefix = NULL;
  char *global_postfix = NULL;
  char full_name[DATA_MAX_NAME_LEN] = {0};

  DEBUG("statsd plugin: submit metric");

  vl.values = values;
  vl.values_len = 1;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "statsd", sizeof (vl.plugin));
  sstrncpy (vl.plugin_instance, conf->node_name, sizeof (vl.plugin_instance));

  global_prefix = (NULL == conf->global_prefix) ? "" : conf->global_prefix;
  global_postfix = (NULL == conf->global_postfix) ? "" : conf->global_postfix;

  switch (metric->type) {
  case STATSD_GAUGE:
    sstrncpy (vl.type, "gauge", sizeof (vl.type));
    type_prefix = (NULL == conf->gauge_prefix) ? "" : conf->gauge_prefix;
    break;
  case STATSD_TIMER:
    sstrncpy (vl.type, "latency", sizeof (vl.type));
    type_prefix = (NULL == conf->timer_prefix) ? "" : conf->timer_prefix;
    break;
  case STATSD_SET:
    sstrncpy (vl.type, "objects", sizeof (vl.type));
    type_prefix = (NULL == conf->set_prefix) ? "" : conf->set_prefix;
    break;
  case STATSD_COUNTER:
    sstrncpy (vl.type, "derive", sizeof (vl.type));
    type_prefix = (NULL == conf->counter_prefix) ? "" : conf->counter_prefix;
    break;
  default:
    ERROR("statsd plugin: unknow metrics type %d", metric->type);
  }

  ssnprintf(full_name, sizeof(full_name), "%s%s%s%s", global_prefix, type_prefix, name, global_postfix);
  DEBUG("statsd plugin: metric name %s", full_name);
  sstrncpy (vl.type_instance, full_name, sizeof (vl.type_instance));

  if (metric->type == STATSD_GAUGE)
    values[0].gauge = (gauge_t) metric->value;
  else if (metric->type == STATSD_TIMER)
  {
    size_t i;
    _Bool have_events = (metric->updates_num > 0);

    /* Make sure all timer metrics share the *same* timestamp. */
    vl.time = cdtime ();

    if (!conf->leave_metrics_name_asis)
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
                 "%s-average", full_name);
    values[0].gauge = have_events
      ? CDTIME_T_TO_DOUBLE (latency_counter_get_average (metric->latency))
      : NAN;
    plugin_dispatch_values (&vl);

    if (conf->timer_lower) {
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
          "%s-lower", full_name);
      values[0].gauge = have_events
        ? CDTIME_T_TO_DOUBLE (latency_counter_get_min (metric->latency))
        : NAN;
      plugin_dispatch_values (&vl);
    }

    if (conf->timer_upper) {
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
          "%s-upper", full_name);
      values[0].gauge = have_events
        ? CDTIME_T_TO_DOUBLE (latency_counter_get_max (metric->latency))
        : NAN;
      plugin_dispatch_values (&vl);
    }

    if (conf->timer_sum) {
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
          "%s-sum", full_name);
      values[0].gauge = have_events
        ? CDTIME_T_TO_DOUBLE (latency_counter_get_sum (metric->latency))
        : NAN;
      plugin_dispatch_values (&vl);
    }

    for (i = 0; i < conf->timer_percentile_num; i++)
    {
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
          "%s-percentile-%.0f", full_name, conf->timer_percentile[i]);
      values[0].gauge = have_events
        ? CDTIME_T_TO_DOUBLE (latency_counter_get_percentile (metric->latency, conf->timer_percentile[i]))
        : NAN;
      plugin_dispatch_values (&vl);
    }

    /* Keep this at the end, since vl.type is set to "gauge" here. The
     * vl.type's above are implicitly set to "latency". */
    if (conf->timer_count) {
      sstrncpy (vl.type, "gauge", sizeof (vl.type));
      ssnprintf (vl.type_instance, sizeof (vl.type_instance),
          "%s-count", full_name);
      values[0].gauge = latency_counter_get_num (metric->latency);
      plugin_dispatch_values (&vl);
    }

    latency_counter_reset (metric->latency);
    return (0);
  }
  else if (metric->type == STATSD_SET)
  {
    if (metric->set == NULL)
      values[0].gauge = 0.0;
    else
      values[0].gauge = (gauge_t) c_avl_size (metric->set);
  }
  else { /* STATSD_COUNTER */
      /*
       * Expand a single value to two metrics:
       *
       * - The absolute counter, as a gauge
       * - A derived rate for this counter
       */
      values[0].derive = (derive_t) metric->value;
      plugin_dispatch_values(&vl);

      sstrncpy(vl.type, "gauge", sizeof (vl.type));
      values[0].gauge = (gauge_t) metric->value;
  }

  return (plugin_dispatch_values (&vl));
} /* }}} int statsd_metric_submit_unsafe */

static int statsd_read (void) /* {{{ */
{
  c_avl_iterator_t *iter;
  char *name;
  statsd_metric_t *metric;

  char **to_be_deleted = NULL;
  size_t to_be_deleted_num = 0;
  size_t i;
  int thread_num = 0;

  DEBUG("statsd plugin: read: threads %zu", statsd_threads_num);

  for (thread_num = 0; thread_num < statsd_threads_num; thread_num++) {
    statsd_config_t *conf = statsd_threads[thread_num].conf;

    pthread_mutex_lock (&conf->metrics_lock);

    if (conf->metrics_tree == NULL)
      {
        DEBUG("statsd plugin: meterics tree is empty");
        pthread_mutex_unlock (&conf->metrics_lock);
        return (0);
      }

    iter = c_avl_get_iterator (conf->metrics_tree);
    while (c_avl_iterator_next (iter, (void *) &name, (void *) &metric) == 0)
      {
        if ((metric->updates_num == 0)
            && ((conf->delete_counters && (metric->type == STATSD_COUNTER))
                || (conf->delete_timers && (metric->type == STATSD_TIMER))
                || (conf->delete_gauges && (metric->type == STATSD_GAUGE))
                || (conf->delete_sets && (metric->type == STATSD_SET))))
          {
            DEBUG ("statsd plugin: Deleting metric \"%s\".", name);
            strarray_add (&to_be_deleted, &to_be_deleted_num, name);
            continue;
          }

        /* Names have a prefix, e.g. "c:", which determines the (statsd) type.
         * Remove this here. */
        statsd_metric_submit_unsafe (conf, name + 2, metric);

        /* Reset the metric. */
        metric->updates_num = 0;
        if (metric->type == STATSD_SET)
          statsd_metric_clear_set_unsafe (metric);
      }
    c_avl_iterator_destroy (iter);

    for (i = 0; i < to_be_deleted_num; i++)
      {
        int status;

        status = c_avl_remove (conf->metrics_tree, to_be_deleted[i],
                               (void *) &name, (void *) &metric);
        if (status != 0)
          {
            ERROR ("stats plugin: c_avl_remove (\"%s\") failed with status %i.",
                   to_be_deleted[i], status);
            continue;
          }

        sfree (name);
        statsd_metric_free (metric);
      }

    pthread_mutex_unlock (&conf->metrics_lock);

    strarray_free (to_be_deleted, to_be_deleted_num);
  }

  return (0);
} /* }}} int statsd_read */

static int statsd_shutdown (void) /* {{{ */
{
  int i;

  DEBUG ("statsd plugin: Shutting down %zu stasd threads.",
         statsd_threads_num);

  for (i = 0; i < statsd_threads_num; i++) {
    pthread_cancel (statsd_threads[i].thr);
    pthread_join (statsd_threads[i].thr, /* retval = */ NULL);
  }

  statsd_threads_free();

  return (0);
} /* }}} int statsd_shutdown */

void module_register (void)
{
  plugin_register_complex_config ("statsd", statsd_config);
  plugin_register_init ("statsd", statsd_init);
  plugin_register_read ("statsd", statsd_read);
  plugin_register_shutdown ("statsd", statsd_shutdown);
}

/* vim: set sw=2 sts=2 et fdm=marker : */
