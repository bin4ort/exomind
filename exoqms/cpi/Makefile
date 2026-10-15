# exocpi — Continuous Process Improvement submodule for exoqms

BUILD := ../build
SRC := src
CC := cc
CFLAGS := -O2 -g -std=c11 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
          -I../src -I../../src -I../../exomind/src -I../../exosched/src -I../../exoflow/src \
          -I../../exodoc/src -I../../exocrawl/src -I../../exocontext/src -I../../exokit/src \
          -MMD -MP

OBJS := $(BUILD)/cpi_main.o $(BUILD)/cpi_pdca.o $(BUILD)/cpi_plan.o $(BUILD)/cpi_do.o \
        $(BUILD)/cpi_check.o $(BUILD)/cpi_act.o $(BUILD)/cpi_qc.o $(BUILD)/cpi_pareto.o \
        $(BUILD)/cpi_fishbone.o $(BUILD)/cpi_histogram.o $(BUILD)/cpi_control.o \
        $(BUILD)/cpi_scatter.o $(BUILD)/cpi_run_chart.o $(BUILD)/cpi_flowchart.o \
        $(BUILD)/cpi_poka.o $(BUILD)/cpi_kanban.o $(BUILD)/cpi_5s.o \
        $(BUILD)/cpi_kaizen.o $(BUILD)/cpi_standard.o $(BUILD)/cpi_report.o \
        $(BUILD)/cpi_checksheet.o

all: $(BUILD)/exocpi

$(BUILD)/exocpi: $(OBJS) $(BUILD)/../http.o $(BUILD)/../util.o $(BUILD)/../exomind.o \
                 $(BUILD)/../model.o $(BUILD)/../checks.o $(BUILD)/../project.o \
                 $(BUILD)/../exo_common.o $(BUILD)/../exo_cli.o
	$(CC) $(CFLAGS) -o $@ $^ -pthread

$(BUILD)/cpi_%.o: $(SRC)/cpi_%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cpi_main.o: $(SRC)/cpi_main.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: all
	bash test/test.sh

clean:
	rm -f $(OBJS) $(BUILD)/exocpi $(BUILD)/cpi_*.d

.PHONY: all test clean

-include $(wildcard $(BUILD)/cpi_*.d)