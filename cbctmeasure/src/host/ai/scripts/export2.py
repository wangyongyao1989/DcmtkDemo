"""export2.py -- ONNX export for the 3x3x3 conv net trained by train2.py.

Graph (opset 17, initializer weights only, no training-only ops):
    feat [1,6,96,96,64] f32
      -> Conv(kernel 3,3,3; pads 1,1,1,1,1,1; strides 1,1,1; W1,b1)
      -> Relu
      -> Conv(3,3,3; pads 1; W2,b2) -> Relu
      -> Conv(3,3,3; pads 1; W3,b3)
      -> Softmax(axis=1) -> prob [1,2,96,96,64] f32

Weight orientation: our numpy W is (Co, Ci, 3, 3, 3) in ONNX correlation
semantics already (Y[o,x,..] = sum W[o,i,a,b,c] X[i,x+a-1,..]), so export is a
direct numpy_helper.from_array -- NO transpose.  The ORT-vs-numpy check in
parity.ort_check is what proves the orientation is right.
"""
import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto


def export_onnx(net, path):
    def conv(name, X, W, B, Y):
        return helper.make_node("Conv", [X, W, B], [Y], name=name + "_conv",
                                kernel_shape=[3, 3, 3], strides=[1, 1, 1],
                                pads=[1, 1, 1, 1, 1, 1])
    inits = [numpy_helper.from_array(np.ascontiguousarray(p, np.float32), n)
             for p, n in [(net.W1, "W1"), (net.b1, "b1"), (net.W2, "W2"),
                          (net.b2, "b2"), (net.W3, "W3"), (net.b3, "b3")]]
    nodes = [
        conv("l1", "feat", "W1", "b1", "z1"),
        helper.make_node("Relu", ["z1"], ["a1"], name="r1"),
        conv("l2", "a1", "W2", "b2", "z2"),
        helper.make_node("Relu", ["z2"], ["a2"], name="r2"),
        conv("l3", "a2", "W3", "b3", "z3"),
        helper.make_node("Softmax", ["z3"], ["prob"], name="sm", axis=1),
    ]
    gin = helper.make_tensor_value_info("feat", TensorProto.FLOAT, [1, 6, 96, 96, 64])
    gout = helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2, 96, 96, 64])
    graph = helper.make_graph(nodes, "teeth_cnn3d", [gin], [gout], inits)
    mdl = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    mdl.ir_version = 8
    onnx.checker.check_model(mdl)
    onnx.save(mdl, path)
    return mdl
