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

int netvirt_parser(struct netvirt_table *inner_table, 
    struct netvirt_table *gid_table, const char *config_path)
{
  FILE *fp;
  char line[MAX_LINE_LEN];
  __u32 guest_id, gre_key, outer_ip, inner_ip;
  char outer_ip_str[16], inner_ip_str[16];
  int ret = 0;

  if (!inner_table || !gid_table || !config_path)
    return -1;

  fp = fopen(config_path, "r");
  if (!fp)
  {
    LOG_ERROR("failed to open config path");
    return -1;
  }

  /* Skip header line */
  if (fgets(line, sizeof(line), fp) == NULL)
  {
    fclose(fp);
    LOG_ERROR("failed to get first header line");
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
      LOG_ERROR("failed to parse outer ip or inner ip");
      fclose(fp);
      return -1;
    }

    /* Add to inner IP table */
    if (netvirt_table_set(inner_table, gre_key, inner_ip,
        guest_id, gre_key, inner_ip, outer_ip) < 0)
    {
      LOG_ERROR("failed to add entry to inner IP table");
      fclose(fp);
      return -1;
    }

    /* Add to guest ID table */
    if (netvirt_table_set(gid_table, guest_id, outer_ip,
        guest_id, gre_key, inner_ip, outer_ip) < 0)
    {
      LOG_ERROR("failed to add entry to guest ID table");
      fclose(fp);
      return -1;
    }
  }

  fclose(fp);
  return ret;
}
