"""Export the KPCN denoiser to ONNX for the RT path.

Input  "input"  [1, 6, H, W]: demodulated light rgb (HDR), albedo rgb
Output "output" [1, 3, H, W]: denoised demodulated light rgb

The net predicts a 5x5 kernel per pixel (softmax-normalized) and applies it to
the noisy light, so the output is always a weighted average of neighbours.
The last conv is zero-initialized: untrained, every kernel is uniform and the
model is a plain 5x5 box filter, which is what --check verifies.
"""
import argparse

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


class KPCN(nn.Module):
    def __init__(self, ch=32, k=5):
        super().__init__()
        self.k = k
        self.net = nn.Sequential(
            nn.Conv2d(6, ch, 3, padding=1), nn.ReLU(),
            nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(),
            nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(),
            nn.Conv2d(ch, ch, 3, padding=1), nn.ReLU(),
            nn.Conv2d(ch, k * k, 1),
        )
        nn.init.zeros_(self.net[-1].weight)
        nn.init.zeros_(self.net[-1].bias)

    def forward(self, x):
        light, albedo = x[:, :3], x[:, 3:]
        feat = torch.cat([torch.log1p(light.clamp(min=0)), albedo], dim=1)
        w = torch.softmax(self.net(feat), dim=1)

        r = self.k // 2
        h, wd = x.shape[2], x.shape[3]
        padded = F.pad(light, (r, r, r, r), mode="replicate")
        out = torch.zeros_like(light)
        for dy in range(self.k):
            for dx in range(self.k):
                i = dy * self.k + dx
                out = out + w[:, i:i + 1] * padded[:, :, dy:dy + h, dx:dx + wd]
        return out


def gpu_weights(model):
    """Flat float32 array in the layout the kpcn_*.comp shaders read:
    conv_in  [tap 9][in 6][out 32], bias 32
    conv_mid [tap 9][in 32][out 32], bias 32   (x3)
    conv_out [in 32][out 28], bias 28          (25 used, padded to vec4)
    """
    convs = [m for m in model.net if isinstance(m, nn.Conv2d)]
    parts = []
    for conv in convs[:-1]:
        w = conv.weight.detach().numpy()                      # [out, in, 3, 3]
        parts += [w.transpose(2, 3, 1, 0).reshape(-1), conv.bias.detach().numpy()]
    w = convs[-1].weight.detach().numpy()[:, :, 0, 0]          # [25, 32]
    w = np.pad(w, ((0, 3), (0, 0))).T                          # [32, 28]
    b = np.pad(convs[-1].bias.detach().numpy(), (0, 3))
    parts += [w.reshape(-1), b]
    return np.concatenate(parts).astype(np.float32)


def gpu_reference(flat, x):
    """numpy forward pass that indexes `flat` exactly like the shaders."""
    light, albedo = x[0, :3], x[0, 3:]
    act = np.concatenate([np.log1p(np.clip(light, 0, None)), albedo]).transpose(1, 2, 0)  # HWC
    h, w = act.shape[:2]
    off = 0
    for cin in (6, 32, 32, 32):
        wt = flat[off:off + 9 * cin * 32].reshape(9, cin, 32); off += 9 * cin * 32
        b = flat[off:off + 32]; off += 32
        p = np.pad(act, ((1, 1), (1, 1), (0, 0)))
        out = np.broadcast_to(b, (h, w, 32)).copy()
        for tap in range(9):
            ky, kx = divmod(tap, 3)
            out += p[ky:ky + h, kx:kx + w] @ wt[tap]
        act = np.maximum(out, 0).astype(np.float16).astype(np.float32)  # fp16 storage
    wt = flat[off:off + 32 * 28].reshape(32, 28); off += 32 * 28
    logits = (act @ wt + flat[off:off + 28])[..., :25]
    e = np.exp(logits - logits.max(-1, keepdims=True))
    k = e / e.sum(-1, keepdims=True)
    pl = np.pad(light.transpose(1, 2, 0), ((2, 2), (2, 2), (0, 0)), mode="edge")
    out = sum(k[..., i:i + 1] * pl[i // 5:i // 5 + h, i % 5:i % 5 + w] for i in range(25))
    return out.transpose(2, 0, 1)[None]


def box5(light):
    p = np.pad(light, ((0, 0), (0, 0), (2, 2), (2, 2)), mode="edge")
    h, w = light.shape[2:]
    return sum(p[:, :, dy:dy + h, dx:dx + w] for dy in range(5) for dx in range(5)) / 25.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="../Run/Data/Models/denoiser_kpcn.onnx")
    ap.add_argument("--weights", help="state_dict .pt; omit for the untrained box-filter model")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    model = KPCN().eval()
    if args.weights:
        model.load_state_dict(torch.load(args.weights, map_location="cpu"))

    dummy = torch.rand(1, 6, 64, 96)
    torch.onnx.export(model, dummy, args.out, input_names=["input"], output_names=["output"],
                      dynamic_axes={"input": {2: "h", 3: "w"}, "output": {2: "h", 3: "w"}},
                      opset_version=17, dynamo=False)
    print("wrote", args.out)

    flat = gpu_weights(model)
    wpath = args.out.replace(".onnx", ".weights")
    flat.tofile(wpath)
    print("wrote", wpath, f"({flat.size} floats)")

    if args.check:
        x = np.random.rand(1, 6, 37, 53).astype(np.float32) * 4
        with torch.no_grad():
            t = model(torch.from_numpy(x)).numpy()
        print("gpu-layout reference vs torch max abs diff:", np.abs(gpu_reference(flat, x) - t).max())
        import onnxruntime as ort
        sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
        x = np.random.rand(1, 6, 123, 217).astype(np.float32) * 4
        y = sess.run(["output"], {"input": x})[0]
        with torch.no_grad():
            t = model(torch.from_numpy(x)).numpy()
        print("ort vs torch max abs diff:", np.abs(y - t).max())
        if not args.weights:
            print("ort vs 5x5 box max abs diff:", np.abs(y - box5(x[:, :3])).max())


if __name__ == "__main__":
    main()
