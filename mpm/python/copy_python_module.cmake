SET(BINARY_DIR "" CACHE STRING "")
SET(SOURCE_DIR "" CACHE STRING "")
FILE(COPY ${SOURCE_DIR}/usrp_mpm/ DESTINATION ${BINARY_DIR}/usrp_mpm
    FILES_MATCHING PATTERN *.py)
