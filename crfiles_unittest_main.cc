// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "mojo/core/embedder/embedder.h"
#include "ui/views/views_test_suite.h"

int main(int argc, char** argv) {
  mojo::core::Init();
  return views::ViewsTestSuite(argc, argv).RunTests();
}
