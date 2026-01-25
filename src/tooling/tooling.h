#ifndef ERKAO_TOOLING_H
#define ERKAO_TOOLING_H

#include <stdbool.h>

typedef enum {
  LINT_RULE_TRAILING_WS = 1u << 0,
  LINT_RULE_TABS = 1u << 1,
  LINT_RULE_INDENT = 1u << 2,
  LINT_RULE_LINE_LENGTH = 1u << 3,
  LINT_RULE_FLOW = 1u << 4,
  LINT_RULE_LEX = 1u << 5
} LintRule;

typedef struct {
  int formatIndent;
  int lintMaxLine;
  unsigned int lintRules;
} ToolingConfig;

void toolingConfigInit(ToolingConfig* config);
bool toolingLoadConfig(const char* path, ToolingConfig* config);
bool toolingApplyFormatRuleset(ToolingConfig* config, const char* name);
bool toolingApplyLintRuleset(ToolingConfig* config, const char* name);
bool toolingApplyLintRules(ToolingConfig* config, const char* rules);

bool formatFileWithConfig(const char* path, bool checkOnly, bool* changed,
                          const ToolingConfig* config);
int lintFileWithConfig(const char* path, const ToolingConfig* config);

bool formatFile(const char* path, bool checkOnly, bool* changed);
int lintFile(const char* path);

#endif
