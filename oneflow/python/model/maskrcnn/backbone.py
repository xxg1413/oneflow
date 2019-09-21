import oneflow as flow
from resnet import ResNet
from fpn import FPN


class Backbone(object):
    def __init__(self, dl_net, cfg):
        self.resnet = ResNet(dl_net, cfg)
        self.fpn = FPN(dl_net, cfg)

    def build(self, in_blob):
        with flow.deprecated.variable_scope("backbone"):
            encoded_tp = flow.transpose(in_blob, perm=[0, 3, 1, 2])
            features = self.resnet.build(encoded_tp)
            layer_features = self.fpn.build(features)
        return layer_features