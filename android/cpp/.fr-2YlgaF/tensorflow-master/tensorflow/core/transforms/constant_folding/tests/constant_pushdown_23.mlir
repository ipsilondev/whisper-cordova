// RUN: tfg-transforms-opt -tfg-constant-folding %s | FileCheck %s

module {
  tfg.func @test() {
    %Const, %ctl = Const device("/job:localhost/replica:0/task:0/device:CPU:0") name("c2") {dtype = f32, value = dense<2.000000e+00> : tensor<2xf32>} : () -> (tensor<2xf32>)
    %Const_0, %ctl_1 = Const device("/job:localhost/replica:0/task:0/device:CPU:0") name("c3") {dtype = f32, value = dense<3.000000e+00> : tensor<2xf32>} : () -> (tensor<2xf32>)
    // CHECK: %[[PLACEHOLDER:.*]], {{.*}} = Placeholder device({{.*}}) name("x")
    %Placeholder, %ctl_2 = Placeholder device("/job:localhost/replica:0/task:0/device:CPU:0") name("x") {dtype = f32, shape = #tf_type.shape<2x2>} : () -> (tensor<2x2xf32>)
    // CHECK: %[[CONST:.*]], %[[CTRL:.*]] = Const{{.*}} device([[DEVICE:.*]]) name("child")
    // CHECK-SAME: value = dense<1.500000e+00>
    %Div, %ctl_3 = Div(%Placeholder, %Const) device("/job:localhost/replica:0/task:0/device:CPU:0") name("child") {T = f32} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Mul(%[[PLACEHOLDER]], %[[CONST]]) device([[DEVICE]]) name("parent")
    %Mul, %ctl_4 = Mul(%Div, %Const_0) device("/job:localhost/replica:0/task:0/device:CPU:0") name("parent") {T = f32} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    // CHECK: %[[DIV_1:.*]], %[[CTRL_DIV:.*]] = Mul({{.*}} name("child_1")
    %Div_1, %ctl_5 = Div(%Placeholder, %Const) device("/job:localhost/replica:0/task:0/device:CPU:0") name("child_1") {T = f32} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    %Const_1, %ctl_7 = Const [%ctl_5] device("/job:localhost/replica:0/task:0/device:CPU:0") name("c4") {dtype = f32, value = dense<3.000000e+00> : tensor<2xf32>} : () -> (tensor<2xf32>)
    // CHECK: Mul(%[[DIV_1]], {{.*}}) {{.*}} name("parent_1")
    %Mul_1, %ctl_6 = Mul(%Div_1, %Const_1) device("/job:localhost/replica:0/task:0/device:CPU:0") name("parent_1") {T = f32} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    return
  }
}
