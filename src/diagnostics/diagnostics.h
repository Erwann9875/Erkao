#ifndef ERKAO_DIAGNOSTICS_H
#define ERKAO_DIAGNOSTICS_H

#include <stddef.h>

#define ERKAO_DIAG_MAX_NAME 48
#define ERKAO_DIAG_MAX_DISTANCE 2

static inline int diag_abs_int(int value) {
  return value < 0 ? -value : value;
}

static inline int diag_min_int(int a, int b) {
  return a < b ? a : b;
}

static inline int diag_min3_int(int a, int b, int c) {
  return diag_min_int(a, diag_min_int(b, c));
}

static inline int diag_edit_distance_limited(const char* a, int aLen,
                                             const char* b, int bLen,
                                             int maxDist) {
  if (!a || !b) return maxDist + 1;
  if (aLen < 0 || bLen < 0) return maxDist + 1;
  if (aLen > ERKAO_DIAG_MAX_NAME || bLen > ERKAO_DIAG_MAX_NAME) return maxDist + 1;
  if (diag_abs_int(aLen - bLen) > maxDist) return maxDist + 1;

  int prev[ERKAO_DIAG_MAX_NAME + 1];
  int curr[ERKAO_DIAG_MAX_NAME + 1];

  for (int j = 0; j <= bLen; j++) {
    prev[j] = j;
  }

  for (int i = 0; i < aLen; i++) {
    curr[0] = i + 1;
    int rowMin = curr[0];
    for (int j = 0; j < bLen; j++) {
      int cost = (a[i] == b[j]) ? 0 : 1;
      int deletion = prev[j + 1] + 1;
      int insertion = curr[j] + 1;
      int substitution = prev[j] + cost;
      int value = diag_min3_int(deletion, insertion, substitution);
      curr[j + 1] = value;
      if (value < rowMin) rowMin = value;
    }
    if (rowMin > maxDist) return maxDist + 1;
    for (int j = 0; j <= bLen; j++) {
      prev[j] = curr[j];
    }
  }

  return prev[bLen];
}

#endif
