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
  /* Bulk add: compared with a sorted, duplicate-free union of the existing
   * entries and the batch, for batches with repeats and existing nodes. */
  for (mode = 0; mode < 6; mode++) {
    idnode_set_t *set = idnode_set_create(1);
    idnode_t **batch = malloc(N * sizeof(*batch));
    size_t before = 0, count = 0, want = 0, j;
    char *member = calloc(N, 1);

    assert(batch && member);
    srand(mode + 1);
    if (mode >= 2)
      for (i = 0; i < N / 3; i++) {
        index = rand() % N;
        if (!member[index]) {
          member[index] = 1;
          idnode_set_add(set, nodes + index, NULL, NULL);
          before++;
        }
      }
    assert(set->is_count == before);
    switch (mode) {
    case 0: count = 0; break;                       /* empty batch */
    case 1: count = N; for (j = 0; j < N; j++) batch[j] = nodes + j; break;
    case 2: count = N; for (j = 0; j < N; j++) batch[j] = nodes + (N - 1 - j); break;
    case 3: count = N; for (j = 0; j < N; j++) batch[j] = nodes + rand() % N; break;
    case 4: count = N; for (j = 0; j < N; j++) batch[j] = nodes + 7; break;
    default: /* only nodes already in the set */
      for (j = 0; j < N; j++)
        if (member[j]) batch[count++] = nodes + j;
      break;
    }
    for (j = 0; j < count; j++)
      member[batch[j] - nodes] = 1;
    idnode_set_add_many(set, batch, count);
    for (j = 0; j < N; j++)
      if (member[j]) expected[want++] = nodes + j;
    assert(set->is_count == want);
    assert(want == 0 || !memcmp(expected, set->is_array, want * sizeof(*expected)));
    for (j = 0; j < want; j++)
      assert(idnode_set_exists(set, expected[j]));
    free(member);
    free(batch);
    free(set->is_array);
    free(set);
  }
  free(expected);
  free(nodes);
  puts("PASS: ascending, descending, duplicate, random, unsorted and filtered "
       "insertion (60,000 entries each); bulk add of empty, new, reversed, "
       "random, repeated and existing batches");
  return 0;
}
