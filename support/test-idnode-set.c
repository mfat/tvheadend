/* Exercise production idnode_set_add with large and adversarial inputs. */
#include "tvheadend.h"
#include "idnode.h"
#include <assert.h>
#include <stdio.h>

static int reject;

/* Only filtering is stubbed; the runner links the real insertion code. */
int
idnode_filter(idnode_t *in, idnode_filter_t *filt, const char *lang)
{
  return reject;
}

static int
compare_nodes(const void *aa, const void *bb)
{
  const idnode_t *a = *(idnode_t *const *)aa;
  const idnode_t *b = *(idnode_t *const *)bb;
  return (a > b) - (a < b);
}

int
main(void)
{
  enum { N = 60000 };
  idnode_t *nodes = calloc(N, sizeof(*nodes));
  idnode_t **expected = malloc((N + 1) * sizeof(*expected));
  int mode, i, index;

  assert(nodes && expected);
  for (mode = 0; mode < 5; mode++) {
    idnode_set_t *set = idnode_set_create(mode != 4);
    idnode_filter_t filter;

    LIST_INIT(&filter);
    reject = 1;
    idnode_set_add(set, nodes, &filter, NULL);
    assert(set->is_count == 0 && set->is_alloc == 0);

    for (i = 0; i < N; i++) {
      switch (mode) {
      case 0: index = i; break;
      case 1: index = N - 1 - i; break;
      case 2: index = 42; break;
      default: index = rand() % N; break;
      }
      expected[i] = nodes + index;
      idnode_set_add(set, expected[i], NULL, NULL);
    }
    if (mode != 4)
      qsort(expected, N, sizeof(*expected), compare_nodes);
    assert(set->is_count == N);
    assert(!memcmp(expected, set->is_array, N * sizeof(*expected)));

    idnode_set_add(set, nodes, &filter, NULL);
    assert(set->is_count == N);
    reject = 0;
    idnode_set_add(set, nodes, &filter, NULL);
    expected[N] = nodes;
    if (mode != 4)
      qsort(expected, N + 1, sizeof(*expected), compare_nodes);
    assert(set->is_count == N + 1);
    assert(!memcmp(expected, set->is_array, (N + 1) * sizeof(*expected)));
    free(set->is_array);
    free(set);
  }
  free(expected);
  free(nodes);
  puts("PASS: ascending, descending, duplicate, random, unsorted and filtered "
       "insertion (60,000 entries each)");
  return 0;
}
