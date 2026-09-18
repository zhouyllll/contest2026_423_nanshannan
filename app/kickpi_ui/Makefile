############################################################################
# packages/demos/contest2026_423_kickpi_ui/Makefile
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/libjpeg-turbo/libjpeg-turbo
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON

PROGNAME  = kickpi_ui
PRIORITY  = $(CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_PRIORITY)
STACKSIZE = $(CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_STACKSIZE)
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_423_KICKPI_UI)

MAINSRC = kickpi_ui_main.c

# 中文字库（scripts/gen-cjk-font.sh 生成，约 430KB）
CSRCS += lv_font_k7_cjk_20.c

include $(APPDIR)/Application.mk
