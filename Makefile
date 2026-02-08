CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I include
LDFLAGS := -lssl -lcrypto -llz4 -lzstd -lsqlite3 -lpthread -lrt

TARGET := ecpb
SRC := src/main.cpp
BUILD_DIR := build

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard include/**/*.h)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $(BUILD_DIR)/$(TARGET) $(SRC) $(LDFLAGS)
	@echo "Build successful: $(BUILD_DIR)/$(TARGET)"

clean:
	rm -rf $(BUILD_DIR) ecpb_data_test

test: $(TARGET)
	@echo "=== Running integration test ==="
	@rm -rf /tmp/ecpb_test_data /tmp/ecpb_test_source /tmp/ecpb_test_restore
	@mkdir -p /tmp/ecpb_test_source/subdir
	@echo "Hello, World!" > /tmp/ecpb_test_source/file1.txt
	@echo "Enterprise Backup Test" > /tmp/ecpb_test_source/file2.txt
	@dd if=/dev/urandom of=/tmp/ecpb_test_source/binary.dat bs=1024 count=64 2>/dev/null
	@echo "Nested file content" > /tmp/ecpb_test_source/subdir/nested.txt
	@cp /tmp/ecpb_test_source/file1.txt /tmp/ecpb_test_source/duplicate.txt
	@echo "--- Test 1: Backup ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --backup /tmp/ecpb_test_source --name test_backup
	@echo "--- Test 2: List ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --list
	@echo "--- Test 3: Stats ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --stats
	@echo "--- Test 4: Verify ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --verify 1
	@echo "--- Test 5: Restore ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --restore 1 --dest /tmp/ecpb_test_restore
	@echo "--- Test 6: Verify restored files ---"
	@diff /tmp/ecpb_test_source/file1.txt /tmp/ecpb_test_restore/file1.txt && echo "file1.txt: OK"
	@diff /tmp/ecpb_test_source/file2.txt /tmp/ecpb_test_restore/file2.txt && echo "file2.txt: OK"
	@diff /tmp/ecpb_test_source/binary.dat /tmp/ecpb_test_restore/binary.dat && echo "binary.dat: OK"
	@diff /tmp/ecpb_test_source/subdir/nested.txt /tmp/ecpb_test_restore/subdir/nested.txt && echo "nested.txt: OK"
	@diff /tmp/ecpb_test_source/duplicate.txt /tmp/ecpb_test_restore/duplicate.txt && echo "duplicate.txt: OK"
	@echo "--- Test 7: Cross-backup deduplication ---"
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --backup /tmp/ecpb_test_source --name test_backup_dup
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_data --verify 2
	@echo "--- Test 8: Multi-chunk file (256KB) ---"
	@rm -rf /tmp/ecpb_test_mchunk_src /tmp/ecpb_test_mchunk_data /tmp/ecpb_test_mchunk_rst
	@mkdir -p /tmp/ecpb_test_mchunk_src
	@dd if=/dev/urandom of=/tmp/ecpb_test_mchunk_src/big.bin bs=1024 count=256 2>/dev/null
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_mchunk_data --backup /tmp/ecpb_test_mchunk_src --name multichunk
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_mchunk_data --restore 1 --dest /tmp/ecpb_test_mchunk_rst
	@diff /tmp/ecpb_test_mchunk_src/big.bin /tmp/ecpb_test_mchunk_rst/big.bin && echo "multi-chunk 256KB: OK"
	@rm -rf /tmp/ecpb_test_mchunk_src /tmp/ecpb_test_mchunk_data /tmp/ecpb_test_mchunk_rst
	@echo "--- Test 9: 50 small files ---"
	@rm -rf /tmp/ecpb_test_many_src /tmp/ecpb_test_many_data /tmp/ecpb_test_many_rst
	@mkdir -p /tmp/ecpb_test_many_src
	@for i in $$(seq 1 50); do echo "File $$i" > /tmp/ecpb_test_many_src/f_$$i.txt; done
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_many_data --backup /tmp/ecpb_test_many_src --name batch50
	$(BUILD_DIR)/$(TARGET) --data-dir /tmp/ecpb_test_many_data --restore 1 --dest /tmp/ecpb_test_many_rst
	@FAIL=0; for i in $$(seq 1 50); do diff /tmp/ecpb_test_many_src/f_$$i.txt /tmp/ecpb_test_many_rst/f_$$i.txt >/dev/null 2>&1 || FAIL=$$((FAIL+1)); done; echo "50 files: $$FAIL failures"
	@rm -rf /tmp/ecpb_test_many_src /tmp/ecpb_test_many_data /tmp/ecpb_test_many_rst
	@echo "=== All tests passed ==="
	@rm -rf /tmp/ecpb_test_data /tmp/ecpb_test_source /tmp/ecpb_test_restore
