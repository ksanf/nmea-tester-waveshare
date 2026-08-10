# Simple alias to ease building from shell
.PHONY: build flash menuconfig clean monitor
build:
	idf.py build
flash:
	idf.py flash -b 921600
monitor:
	idf.py monitor
clean:
	idf.py fullclean
