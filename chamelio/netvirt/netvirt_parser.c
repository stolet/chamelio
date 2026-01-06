#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "netvirt.h"
#include "log.h"

#define MAX_LINE_LEN 256

int parse_ipv4(const char *s)
{
  __u32 ip;
  if (inet_pton(AF_INET, s, &ip) != 1) 
  {
    return -1;
  }
  
  return ntohl(ip);
}

int netvirt_parser(struct ip_table *ip_tbl, struct gre_table *gre_tbl,
    const char *config_path)
{
  FILE *fp;
  char line[MAX_LINE_LEN];
  __u32 guest_id, gre_key, outer_ip, inner_ip;
  char outer_ip_str[16], inner_ip_str[16];
  int ret = 0;

  if (!ip_tbl || !gre_tbl || !config_path)
    return -1;

  fp = fopen(config_path, "r");
  if (!fp)
  {
    return -1;
  }

  /* Skip header line */
  if (fgets(line, sizeof(line), fp) == NULL)
  {
    fclose(fp);
    return -1;
  }

  /* Parse each data line */
  while (fgets(line, sizeof(line), fp) != NULL)
  {
    /* Remove trailing newline */
    line[strcspn(line, "\n")] = '\0';

    /* Parse CSV: GUEST_ID, GRE_KEY, OUTER_IP, INNER_IP */
    if (sscanf(line, "%u, %u, %15[^,], %15[^\n]", &guest_id, &gre_key,
               outer_ip_str, inner_ip_str) != 4)
      continue;

    outer_ip = parse_ipv4(outer_ip_str);
    inner_ip = parse_ipv4(inner_ip_str);

    if (outer_ip == 0 || inner_ip == 0)
    {
      ret = -1;
      break;
    }

    /* Add to IP table */
    if (netvirt_ip_set(ip_tbl, gre_key, inner_ip, outer_ip) < 0)
    {
      ret = -1;
      break;
    }

    /* Add to GRE table */
    if (netvirt_gre_set(gre_tbl, outer_ip, guest_id, gre_key) < 0)
    {
      ret = -1;
      break;
    }
  }

  fclose(fp);
  return ret;
}
