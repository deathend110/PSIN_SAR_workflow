# Demo Simple Infer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone `demo/` executable that scans an input directory, skips non-`512x512` images, runs one inference per valid image, and writes `restore.png` plus `mask_class.png` for each image.

**Architecture:** `demo` is source-level independent from `main` and only shares third-party dependencies from `deps/**`. The program stays single-threaded, uses a tiny YAML config, hardcodes the current ZG330 backend/device defaults in code, and keeps responsibilities split into config loading, image collection, model running, result writing, and a thin `main()`.

**Tech Stack:** C++17, CMake, OpenCV, icraft XRT/XIR, ZG330 backend, repo-local `deps/**`

---

## File Map

### Create
- `demo/CMakeLists.txt`: standalone build entry for `demo_simple_infer` and 3 host-side unit tests
- `demo/README.md`: build, run, smoke-test instructions
- `demo/configs/demo_infer.yaml`: minimal runtime config
- `demo/include/demo_config.hpp`: `DemoConfig` + `LoadDemoConfig(...)`
- `demo/include/image_collector.hpp`: image enumeration and validation API
- `demo/include/model_runner.hpp`: `InferenceResult` + `ModelRunner`
- `demo/include/result_writer.hpp`: `WriteOutputs(...)`
- `demo/src/demo_config.cpp`: tiny YAML parsing and field validation
- `demo/src/image_collector.cpp`: directory scan, grayscale load, `512x512` validation
- `demo/src/model_runner.cpp`: hardcoded ZG330 device/session creation and single-image inference
- `demo/src/result_writer.cpp`: `restore.png` and `mask_class.png` writing
- `demo/src/demo_main.cpp`: orchestration entrypoint
- `demo/tests/demo_config_test.cpp`: config parser regression tests
- `demo/tests/image_collector_test.cpp`: enumeration and size validation tests
- `demo/tests/result_writer_test.cpp`: output write tests

### No Changes
- `main/src/**`
- `main/include/**`
- `deps/**`
- top-level `CMakeLists.txt`

### Fixed Assumptions
- backend is fixed in code to `zg330`
- device URL is fixed in code to `axi://zg330aiu?npu=0x40000000&dma=0x80000000`
- supported input extensions: `.png`, `.jpg`, `.jpeg`, `.bmp`
- unreadable or wrong-sized images are logged and skipped
- `mask_class.png` stores class IDs directly as `uint8` values `1~6`

---

### Task 1: Scaffold Standalone Build

**Files:**
- Create: `demo/CMakeLists.txt`
- Create: `demo/README.md`
- Create: `demo/configs/demo_infer.yaml`

- [ ] **Step 1: Write the failing build scaffold**

```cmake
cmake_minimum_required(VERSION 3.16)
project(DemoSimpleInfer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DEMO_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps)
set(DEMO_COMMON_INCLUDE_DIRS
    ${DEMO_DEPS_DIR}/modelzoo_utils/include
    ${DEMO_DEPS_DIR}/thirdparty/include
    ${DEMO_DEPS_DIR}/thirdparty/include/Eigen-3.4.0
    ${DEMO_DEPS_DIR}/thirdparty/include/Eigen-3.4.0/Eigen
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
option(DEMO_BUILD_TESTS "Build demo unit tests" ON)
find_package(Icraft-HostBackend REQUIRED)
find_package(icraft-zg330backend REQUIRED)
add_library(demo_support STATIC src/demo_config.cpp src/image_collector.cpp src/result_writer.cpp)
target_include_directories(demo_support PUBLIC ${DEMO_COMMON_INCLUDE_DIRS})
target_link_libraries(demo_support PUBLIC opencv_imgcodecs opencv_imgproc opencv_core fmt pthread dl zlib ssl crypto atomic $<$<CXX_COMPILER_ID:GNU>:stdc++fs>)
add_executable(demo_simple_infer src/demo_main.cpp src/model_runner.cpp)
target_include_directories(demo_simple_infer PRIVATE ${DEMO_COMMON_INCLUDE_DIRS})
target_link_libraries(demo_simple_infer PRIVATE demo_support Icraft::HostBackend Icraft::ZG330Backend avformat avdevice avfilter avcodec avutil swscale swresample opencv_imgcodecs opencv_imgproc opencv_core fmt pthread dl zlib ssl crypto atomic $<$<CXX_COMPILER_ID:GNU>:stdc++fs>)
if(DEMO_BUILD_TESTS)
  enable_testing()
  add_executable(demo_config_test tests/demo_config_test.cpp)
  target_include_directories(demo_config_test PRIVATE ${DEMO_COMMON_INCLUDE_DIRS})
  target_link_libraries(demo_config_test PRIVATE demo_support)
  add_test(NAME demo_config_test COMMAND demo_config_test)
  add_executable(image_collector_test tests/image_collector_test.cpp)
  target_include_directories(image_collector_test PRIVATE ${DEMO_COMMON_INCLUDE_DIRS})
  target_link_libraries(image_collector_test PRIVATE demo_support)
  add_test(NAME image_collector_test COMMAND image_collector_test)
  add_executable(result_writer_test tests/result_writer_test.cpp)
  target_include_directories(result_writer_test PRIVATE ${DEMO_COMMON_INCLUDE_DIRS})
  target_link_libraries(result_writer_test PRIVATE demo_support)
  add_test(NAME result_writer_test COMMAND result_writer_test)
endif()
```

```markdown
# Demo Simple Infer

## Build
`cmake -S demo -B demo/build`
`cmake --build demo/build`

## Run
`demo/build/demo_simple_infer demo/configs/demo_infer.yaml`
```

```yaml
demo:
  input_dir: ./io/demo_input
  output_dir: ./io/demo_output
  json_path: ./models/model.json
  raw_path: ./models/model.raw
```

- [ ] **Step 2: Run configure to verify it fails**

Run: `cmake -S demo -B demo/build -DDEMO_BUILD_TESTS=ON`
Expected: FAIL because the referenced `demo/src/*.cpp` and `demo/tests/*.cpp` files do not exist yet.

- [ ] **Step 3: Add placeholders for all referenced files**

```cpp
// demo/include/demo_config.hpp
#pragma once
namespace demo { struct DemoConfig; }

// demo/include/image_collector.hpp
#pragma once

// demo/include/model_runner.hpp
#pragma once

// demo/include/result_writer.hpp
#pragma once

// demo/src/demo_config.cpp
#include "demo_config.hpp"

// demo/src/image_collector.cpp
#include "image_collector.hpp"

// demo/src/model_runner.cpp
#include "model_runner.hpp"

// demo/src/result_writer.cpp
#include "result_writer.hpp"

// demo/src/demo_main.cpp
int main() { return 0; }

// demo/tests/demo_config_test.cpp
int main() { return 0; }

// demo/tests/image_collector_test.cpp
int main() { return 0; }

// demo/tests/result_writer_test.cpp
int main() { return 0; }
```

- [ ] **Step 4: Run configure and build placeholders**

Run: `cmake -S demo -B demo/build -DDEMO_BUILD_TESTS=ON && cmake --build demo/build --target demo_config_test image_collector_test result_writer_test`
Expected: PASS; placeholder test targets build.

- [ ] **Step 5: Commit**

```bash
git add demo/CMakeLists.txt demo/README.md demo/configs/demo_infer.yaml demo/include demo/src demo/tests
git commit -m "build: scaffold standalone demo infer project"
```

### Task 2: Implement Minimal Config Loader

**Files:**
- Modify: `demo/include/demo_config.hpp`
- Modify: `demo/src/demo_config.cpp`
- Modify: `demo/tests/demo_config_test.cpp`

- [ ] **Step 1: Write the failing config test**

```cpp
#include "demo_config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void Fail(const std::string &m){ std::cerr << "demo_config_test failed: " << m << '\n'; std::exit(1);} 
void Expect(bool ok,const std::string &m){ if(!ok) Fail(m);} 
std::filesystem::path MakeTempFile(const std::string &name,const std::string &content){
  const auto root=std::filesystem::temp_directory_path()/"demo_config_test"; std::filesystem::create_directories(root);
  const auto path=root/name; std::ofstream(path,std::ios::binary|std::ios::trunc)<<content; return path; }
void TestLoadsMinimalYaml(){
  const auto path=MakeTempFile("ok.yaml","demo:\n  input_dir: ./input\n  output_dir: ./output\n  json_path: ./model.json\n  raw_path: ./model.raw\n");
  const demo::DemoConfig cfg=demo::LoadDemoConfig(path);
  Expect(cfg.input_dir=="./input","input_dir mismatch"); Expect(cfg.output_dir=="./output","output_dir mismatch");
  Expect(cfg.json_path=="./model.json","json_path mismatch"); Expect(cfg.raw_path=="./model.raw","raw_path mismatch"); }
void TestMissingFieldThrows(){
  const auto path=MakeTempFile("missing.yaml","demo:\n  input_dir: ./input\n  output_dir: ./output\n  json_path: ./model.json\n");
  try { (void)demo::LoadDemoConfig(path); Fail("expected missing raw_path to throw"); }
  catch(const std::runtime_error &e){ Expect(std::string(e.what()).find("raw_path")!=std::string::npos,"exception should mention raw_path"); }} }
int main(){ TestLoadsMinimalYaml(); TestMissingFieldThrows(); std::cout << "demo_config_test passed\n"; return 0; }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build demo/build --target demo_config_test && ctest --test-dir demo/build -R demo_config_test --output-on-failure`
Expected: FAIL because `DemoConfig` and `LoadDemoConfig(...)` are not implemented yet.

- [ ] **Step 3: Write the minimal loader**

```cpp
// demo/include/demo_config.hpp
#pragma once
#include <filesystem>
namespace demo {
struct DemoConfig { std::filesystem::path input_dir; std::filesystem::path output_dir; std::filesystem::path json_path; std::filesystem::path raw_path; };
DemoConfig LoadDemoConfig(const std::filesystem::path &config_path);
}

// demo/src/demo_config.cpp
#include "demo_config.hpp"
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace demo { namespace {
std::string Trim(const std::string &v){ const auto b=v.find_first_not_of(" \t\r\n"); if(b==std::string::npos) return {}; const auto e=v.find_last_not_of(" \t\r\n"); return v.substr(b,e-b+1);} 
std::unordered_map<std::string,std::string> LoadSimpleYaml(const std::filesystem::path &path){
  std::ifstream input(path); if(!input) throw std::runtime_error("failed to open config: "+path.string());
  std::unordered_map<std::string,std::string> values; std::string line,scope;
  while(std::getline(input,line)){ const auto hash=line.find('#'); if(hash!=std::string::npos) line=line.substr(0,hash); const auto trimmed=Trim(line); if(trimmed.empty()) continue; if(trimmed.back()==':'){ scope=Trim(trimmed.substr(0,trimmed.size()-1)); continue; } const auto colon=trimmed.find(':'); if(colon==std::string::npos) continue; values[scope+"."+Trim(trimmed.substr(0,colon))]=Trim(trimmed.substr(colon+1)); }
  return values; }
std::filesystem::path RequirePath(const std::unordered_map<std::string,std::string> &values,const std::string &key){ const auto it=values.find(key); if(it==values.end()||Trim(it->second).empty()) throw std::runtime_error("missing required config field: "+key); return it->second; }
} DemoConfig LoadDemoConfig(const std::filesystem::path &config_path){ const auto values=LoadSimpleYaml(config_path); DemoConfig cfg; cfg.input_dir=RequirePath(values,"demo.input_dir"); cfg.output_dir=RequirePath(values,"demo.output_dir"); cfg.json_path=RequirePath(values,"demo.json_path"); cfg.raw_path=RequirePath(values,"demo.raw_path"); return cfg; } }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build demo/build --target demo_config_test && ctest --test-dir demo/build -R demo_config_test --output-on-failure`
Expected: PASS with `demo_config_test passed`.

- [ ] **Step 5: Commit**

```bash
git add demo/include/demo_config.hpp demo/src/demo_config.cpp demo/tests/demo_config_test.cpp
git commit -m "feat: add minimal demo config loader"
```
### Task 3: Implement Image Collection and Validation

**Files:**
- Modify: `demo/include/image_collector.hpp`
- Modify: `demo/src/image_collector.cpp`
- Modify: `demo/tests/image_collector_test.cpp`

- [ ] **Step 1: Write the failing image collector tests**

```cpp
#include "image_collector.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
namespace {
void Fail(const std::string &m){ std::cerr << "image_collector_test failed: " << m << '\n'; std::exit(1);} 
void Expect(bool ok,const std::string &m){ if(!ok) Fail(m);} 
std::filesystem::path MakeRoot(){ const auto root=std::filesystem::temp_directory_path()/"image_collector_test"; std::filesystem::remove_all(root); std::filesystem::create_directories(root); return root; }
void WriteImage(const std::filesystem::path &path,int w,int h){ cv::Mat img(h,w,CV_8UC1,cv::Scalar(7)); if(!cv::imwrite(path.string(),img)) Fail("failed to write temp image"); }
void TestCollectsSupportedImages(){
  const auto root=MakeRoot();
  WriteImage(root/"a.png",512,512); WriteImage(root/"b.jpg",512,512); WriteImage(root/"bad.png",300,512);
  std::ofstream(root/"note.txt") << "ignore";
  const auto items=demo::CollectImages(root);
  Expect(items.size()==3,"should collect supported image extensions only");
}
void TestLoadsValidAndSkipsInvalidSize(){
  const auto root=MakeRoot();
  WriteImage(root/"ok.png",512,512); WriteImage(root/"bad.png",511,512);
  const auto results=demo::LoadValidImages(demo::CollectImages(root));
  Expect(results.valid.size()==1,"exactly one image should be valid");
  Expect(results.skipped.size()==1,"exactly one image should be skipped");
  Expect(results.valid.front().image.rows==512 && results.valid.front().image.cols==512,"valid image must stay 512x512");
}
}
int main(){ TestCollectsSupportedImages(); TestLoadsValidAndSkipsInvalidSize(); std::cout << "image_collector_test passed\n"; return 0; }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build demo/build --target image_collector_test && ctest --test-dir demo/build -R image_collector_test --output-on-failure`
Expected: FAIL because collector APIs are not implemented yet.

- [ ] **Step 3: Implement the collector API**

```cpp
// demo/include/image_collector.hpp
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <opencv2/core/mat.hpp>
namespace demo {
struct InputImage { std::filesystem::path path; cv::Mat image; };
struct SkippedImage { std::filesystem::path path; std::string reason; };
struct LoadImagesResult { std::vector<InputImage> valid; std::vector<SkippedImage> skipped; };
std::vector<std::filesystem::path> CollectImages(const std::filesystem::path &input_dir);
LoadImagesResult LoadValidImages(const std::vector<std::filesystem::path> &paths);
}
```

Implementation rules for `demo/src/image_collector.cpp`:
- iterate `std::filesystem::directory_iterator` only one level deep
- normalize extension matching to lowercase
- accept `.png`, `.jpg`, `.jpeg`, `.bmp`
- sort returned file paths lexicographically for deterministic output order
- load with `cv::imread(..., cv::IMREAD_GRAYSCALE)`
- unreadable files go to `skipped`
- non-`512x512` files go to `skipped` with a reason string containing `512x512`
- valid files stay single-channel `CV_8UC1`

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build demo/build --target image_collector_test && ctest --test-dir demo/build -R image_collector_test --output-on-failure`
Expected: PASS with `image_collector_test passed`.

- [ ] **Step 5: Commit**

```bash
git add demo/include/image_collector.hpp demo/src/image_collector.cpp demo/tests/image_collector_test.cpp
git commit -m "feat: add demo image collection and validation"
```

### Task 4: Implement Result Writing

**Files:**
- Modify: `demo/include/result_writer.hpp`
- Modify: `demo/src/result_writer.cpp`
- Modify: `demo/tests/result_writer_test.cpp`

- [ ] **Step 1: Write the failing writer test**

```cpp
#include "result_writer.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
namespace {
void Fail(const std::string &m){ std::cerr << "result_writer_test failed: " << m << '\n'; std::exit(1);} 
void Expect(bool ok,const std::string &m){ if(!ok) Fail(m);} 
void TestWritesBothOutputs(){
  const auto root=std::filesystem::temp_directory_path()/"result_writer_test"; std::filesystem::remove_all(root);
  cv::Mat restore(512,512,CV_8UC1,cv::Scalar(120)); cv::Mat mask(512,512,CV_8UC1,cv::Scalar(3));
  demo::WriteOutputs(root,"sample",restore,mask);
  const auto restore_path=root/"sample"/"restore.png";
  const auto mask_path=root/"sample"/"mask_class.png";
  Expect(std::filesystem::exists(restore_path),"restore.png missing");
  Expect(std::filesystem::exists(mask_path),"mask_class.png missing");
  const cv::Mat restore_loaded=cv::imread(restore_path.string(),cv::IMREAD_UNCHANGED);
  const cv::Mat mask_loaded=cv::imread(mask_path.string(),cv::IMREAD_UNCHANGED);
  Expect(restore_loaded.type()==CV_8UC1,"restore must be single-channel u8");
  Expect(mask_loaded.type()==CV_8UC1,"mask must be single-channel u8");
  Expect(mask_loaded.at<unsigned char>(0,0)==3,"mask class value mismatch");
}
}
int main(){ TestWritesBothOutputs(); std::cout << "result_writer_test passed\n"; return 0; }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build demo/build --target result_writer_test && ctest --test-dir demo/build -R result_writer_test --output-on-failure`
Expected: FAIL because writer APIs are not implemented yet.

- [ ] **Step 3: Implement the writer**

```cpp
// demo/include/result_writer.hpp
#pragma once
#include <filesystem>
#include <string_view>
#include <opencv2/core/mat.hpp>
namespace demo {
void WriteOutputs(const std::filesystem::path &output_dir,std::string_view stem,const cv::Mat &restore,const cv::Mat &mask_class);
}
```

Implementation rules for `demo/src/result_writer.cpp`:
- create `output_dir / stem`
- validate both mats are `512x512` and `CV_8UC1`; otherwise throw `std::runtime_error`
- save exactly two files: `restore.png` and `mask_class.png`
- use `cv::imwrite` and throw on failure
- do not create colorized masks or extra metadata files

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build demo/build --target result_writer_test && ctest --test-dir demo/build -R result_writer_test --output-on-failure`
Expected: PASS with `result_writer_test passed`.

- [ ] **Step 5: Commit**

```bash
git add demo/include/result_writer.hpp demo/src/result_writer.cpp demo/tests/result_writer_test.cpp
git commit -m "feat: add demo output writer"
```
### Task 5: Implement Standalone Model Runner

**Files:**
- Modify: `demo/include/model_runner.hpp`
- Modify: `demo/src/model_runner.cpp`
- Modify: `demo/CMakeLists.txt`

- [ ] **Step 1: Define the public runner API first**

```cpp
// demo/include/model_runner.hpp
#pragma once
#include <filesystem>
#include <memory>
#include <opencv2/core/mat.hpp>
namespace demo {
struct InferenceResult { cv::Mat restore; cv::Mat mask_class; };
class ModelRunner {
public:
  ModelRunner(const std::filesystem::path &json_path,const std::filesystem::path &raw_path);
  ~ModelRunner();
  ModelRunner(ModelRunner&&) noexcept;
  ModelRunner& operator=(ModelRunner&&) noexcept;
  ModelRunner(const ModelRunner&) = delete;
  ModelRunner& operator=(const ModelRunner&) = delete;
  InferenceResult Run(const cv::Mat &input_gray_512);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
```

Reasoning:
- hide XRT/XIR/backend-heavy headers from most translation units
- keep ownership explicit with `std::unique_ptr`
- preserve RAII and keep `demo_main.cpp` tiny

- [ ] **Step 2: Implement the runtime path in `demo/src/model_runner.cpp`**

Implementation requirements:
- fixed backend string: `zg330`
- fixed device URL: `axi://zg330aiu?npu=0x40000000&dma=0x80000000`
- open the device in constructor and close it in `Impl` destructor
- load graph/subgraph from `json_path` + `raw_path`
- create one session and reuse it for all input images
- validate model input shape is compatible with one `512x512` single-channel image
- convert `CV_8UC1` input into normalized float tensor in `[0,1]`
- run forward once per image
- convert restore output to grayscale `CV_8UC1`
- convert segmentation logits to class-id `CV_8UC1` with values `1~6`
- throw `std::runtime_error` on any initialization or inference failure

Suggested internal helpers inside `model_runner.cpp`:
- `cv::Mat NormalizeToFloat32(const cv::Mat &gray_u8)`
- `cv::Mat RestoreToGrayU8(const float *data,int rows,int cols)`
- `cv::Mat LogitsToMaskClassU8(const float *logits,int classes,int rows,int cols)`
- `void ValidateInputImage(const cv::Mat &input)`

Keep it simple:
- single-threaded only
- no patch planner
- no snapshot publishing
- no HDMI or UI render path
- no dependency on `main/src/**`

- [ ] **Step 3: Adjust build linkage if compile errors reveal missing backend libs**

Possible additions in `demo/CMakeLists.txt` only if needed after a real compile:
- extra include directories for XRT/XIR headers
- extra link targets already available in the repo toolchain

Constraint:
- do not pull in `main` sources just to satisfy linkage

- [ ] **Step 4: Build the executable**

Run: `cmake --build demo/build --target demo_simple_infer`
Expected: PASS; the standalone executable links successfully.

- [ ] **Step 5: Commit**

```bash
git add demo/include/model_runner.hpp demo/src/model_runner.cpp demo/CMakeLists.txt
git commit -m "feat: add standalone demo model runner"
```

### Task 6: Wire the Main Flow

**Files:**
- Modify: `demo/src/demo_main.cpp`
- Modify: `demo/README.md`

- [ ] **Step 1: Implement the orchestration logic**

```cpp
int main(int argc,char **argv){
  if(argc!=2){ std::cerr << "usage: demo_simple_infer <config.yaml>\n"; return 1; }
  const demo::DemoConfig cfg=demo::LoadDemoConfig(argv[1]);
  const auto paths=demo::CollectImages(cfg.input_dir);
  const auto loaded=demo::LoadValidImages(paths);
  demo::ModelRunner runner(cfg.json_path,cfg.raw_path);
  for(const auto &item:loaded.valid){
    const demo::InferenceResult result=runner.Run(item.image);
    demo::WriteOutputs(cfg.output_dir,item.path.stem().string(),result.restore,result.mask_class);
    std::cout << "processed: " << item.path.filename().string() << '\n';
  }
  for(const auto &skip:loaded.skipped){
    std::cout << "skipped: " << skip.path.filename().string() << " reason=" << skip.reason << '\n';
  }
  std::cout << "done valid=" << loaded.valid.size() << " skipped=" << loaded.skipped.size() << '\n';
  return 0;
}
```

Behavior requirements:
- fail fast on config/model/device initialization errors
- continue across bad images once collection has started
- create `output_dir` lazily during writing
- do not suppress exceptions silently; print the message and return non-zero from `main()`

- [ ] **Step 2: Update `demo/README.md` with real instructions**

Required sections:
- purpose and non-goals
- expected input image rules
- example config
- build commands
- run command
- output directory layout
- note that `mask_class.png` stores class IDs `1~6`

- [ ] **Step 3: Build all targets and run host-side tests**

Run: `cmake --build demo/build && ctest --test-dir demo/build --output-on-failure`
Expected: PASS for `demo_config_test`, `image_collector_test`, `result_writer_test`.

- [ ] **Step 4: Commit**

```bash
git add demo/src/demo_main.cpp demo/README.md
git commit -m "feat: wire standalone demo inference flow"
```

### Task 7: Manual Smoke Validation

**Files:**
- Modify: none unless smoke testing reveals a bug

- [ ] **Step 1: Prepare a tiny manual dataset**

Create:
- one valid `512x512` grayscale image
- one wrong-size image such as `300x512`
- place both under the configured `input_dir`

- [ ] **Step 2: Run the executable end-to-end**

Run: `demo/build/demo_simple_infer demo/configs/demo_infer.yaml`
Expected console shape:
- one `processed: ...`
- one `skipped: ... reason=...512x512...`
- final summary `done valid=1 skipped=1`

- [ ] **Step 3: Inspect output files**

Verify:
- `output_dir/<stem>/restore.png` exists
- `output_dir/<stem>/mask_class.png` exists
- `mask_class.png` is single-channel `uint8`
- observed pixel values stay within `1~6`

- [ ] **Step 4: Optional helper command for pixel sanity**

```bash
python - <<'PY'
from pathlib import Path
import cv2
import numpy as np
mask = cv2.imread(str(Path('io/demo_output/sample/mask_class.png')), cv2.IMREAD_UNCHANGED)
print(mask.dtype, mask.shape, int(mask.min()), int(mask.max()), sorted(np.unique(mask).tolist())[:16])
PY
```

Expected: dtype `uint8`, shape `(512, 512)`, min/max within `1..6`.

- [ ] **Step 5: Commit only if smoke test exposed and fixed a bug**

```bash
git add demo
git commit -m "fix: address demo smoke test findings"
```

## Review Checklist

- [ ] `demo` still has no source dependency on `main/src/**`
- [ ] config stays limited to `input_dir`, `output_dir`, `json_path`, `raw_path`
- [ ] collector remains single-level and deterministic
- [ ] non-`512x512` images are skipped, not resized
- [ ] writer emits only `restore.png` and `mask_class.png`
- [ ] `mask_class.png` stores raw class IDs, not a color visualization
- [ ] model runner owns device/session with RAII and reuses one session
- [ ] host-side tests cover config, image filtering, and writer behavior
- [ ] final manual smoke run confirms console summary and file outputs

## Notes for the Implementer

- If the backend API names differ from the sketch above, adapt locally inside `demo/src/model_runner.cpp` and keep the public API unchanged.
- If model output layout differs from the assumed restore-plus-logits format, stop and document the exact tensor names/shapes before widening the plan.
- If unit-testing the real backend becomes practical later, add a separate gated integration test target instead of polluting the fast host-side tests.
