set(TARGET_LIB sample_component)

add_library(${TARGET_LIB} MODULE sample_component.cc)

target_include_directories(${TARGET_LIB} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/includes
  ${CMAKE_HOME_DIRECTORY}/3rd-party/opencv-3.4.20/include
  ${CMAKE_HOME_DIRECTORY}/3rd-party/tflite/include)

# 선명도 게이트(라플라시안 분산으로 모션블러 프레임 배제) → OpenCV 정적 링크.
# 아카이브 순서: opencv 모듈 먼저, 그 다음 의존 3rd-party(jpeg/png/zlib 등).
target_link_directories(${TARGET_LIB} PRIVATE
  ${CMAKE_HOME_DIRECTORY}/3rd-party/opencv-3.4.20/lib
  ${CMAKE_HOME_DIRECTORY}/3rd-party/opencv-3.4.20/share/OpenCV/3rdparty/lib
  ${CMAKE_HOME_DIRECTORY}/3rd-party/tflite/lib)

target_link_libraries(${TARGET_LIB} PRIVATE stdlib
  opencv_photo opencv_imgcodecs opencv_imgproc opencv_core
  libjpeg-turbo libpng libjasper libprotobuf ittnotify zlib pthread dl
  tensorflowlite_c)

# tensorflowlite_c 는 동적 라이브러리(.so) — OpenCV(정적)와 달리 런타임에 별도 파일로
# 필요. rpath=$ORIGIN 으로 "이 .so 와 같은 폴더"에서 찾게 하고, 아래 install(FILES...)
# 로 libsample_component.so 와 같은 폴더에 함께 배치한다(패키징 시 .cap 에 포함됨).
set_target_properties(${TARGET_LIB} PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN")

install (TARGETS ${TARGET_LIB} DESTINATION ${CMAKE_HOME_DIRECTORY}/libs/${TARGET_LIB})
install (FILES ${CMAKE_HOME_DIRECTORY}/3rd-party/tflite/lib/libtensorflowlite_c.so
         DESTINATION ${CMAKE_HOME_DIRECTORY}/libs/${TARGET_LIB})
install (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/manifests/ DESTINATION ${CMAKE_HOME_DIRECTORY}/libs/${TARGET_LIB} FILES_MATCHING PATTERN "*manifest.json")
install (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/manifests/ DESTINATION ${CMAKE_HOME_DIRECTORY}/res/models FILES_MATCHING PATTERN "*instance*.json" PATTERN "*attribute*.json")

# 번호판 OCR 모델(tinyLPR 단독) — TFLite 파일 + 문자셋(label)만 패키징 (Multi-line 제거).
install (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ocr_models/ DESTINATION ${CMAKE_HOME_DIRECTORY}/res/ocr_models
         FILES_MATCHING PATTERN "kr_lpr*" PATTERN "registered_plates.txt")
