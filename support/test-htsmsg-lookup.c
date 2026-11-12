#include "tvheadend.h"
#include "htsmsg.h"
#include <assert.h>
/* Compare the real lookup against an independent linear search, including
 * empty names, shared prefixes, duplicates, null arguments and misses. */
int main(void) {
 htsmsg_t *m=htsmsg_create_map();
 htsmsg_add_str(m,"start","first");
 htsmsg_add_str(m,"stop","second");
 htsmsg_add_str(m,"start","duplicate");
 htsmsg_add_str(m,"","empty");
 assert(!strcmp(htsmsg_field_find(m,"start")->hmf_str,"first"));
 assert(!strcmp(htsmsg_field_find(m,"stop")->hmf_str,"second"));
 assert(!strcmp(htsmsg_field_find(m,"")->hmf_str,"empty"));
 assert(!htsmsg_field_find(m,"sta"));
 assert(!htsmsg_field_find(m,"starting"));
 assert(!htsmsg_field_find(m,"absent"));
 assert(!htsmsg_field_find(NULL,"start"));
 assert(!htsmsg_field_find(m,NULL));
 for (int i=0;i<10000;i++) {
  char name[80];
  snprintf(name,sizeof(name),"%cfield-%d",'a'+i%26,i%500);
  htsmsg_field_t *expected=NULL,*f;
  TAILQ_FOREACH(f,&m->hm_fields,hmf_link)
   if (!strcmp(htsmsg_field_name(f),name)) { expected=f; break; }
  assert(htsmsg_field_find(m,name)==expected);
  if(i<500) htsmsg_add_str(m,name,"value");
 }
 htsmsg_t *list=htsmsg_create_list();
 htsmsg_add_str(list,NULL,"list item");
 assert(htsmsg_field_find(list,"")==TAILQ_FIRST(&list->hm_fields));
 assert(!htsmsg_field_find(list,"named"));
 htsmsg_destroy(list);
 htsmsg_destroy(m);
 return 0;
}
