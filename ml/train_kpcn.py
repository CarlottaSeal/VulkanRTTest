"""Train the KPCN on a denoise_capture set.

    python train_kpcn.py --data ../Run/Data/DenoiseSet --out runs/kpcn.pt

Samples whose noisy frame is already close to the clean one (lights don't
reach that pose, PSNR > 40 dB) are dropped. The last 10% of the remaining
poses are held out. Loss is L1 on log1p(light), so bright and dark regions
weigh about the same; reported PSNRs are on linear demodulated light
(peak 1), for noisy input, the untrained box filter, and the model.
"""
import argparse
import time

import numpy as np
import torch
import torch.nn.functional as F

from export_kpcn import KPCN


def load_set(d):
    w, h, _ = map(int, open(f"{d}/info.txt").read().split())
    n = len(open(f"{d}/poses.txt").read().splitlines())
    samples = []
    for i in range(n):
        p = f"{d}/{i:04d}"
        noisy = np.fromfile(p + "_noisy.f16", np.float16).reshape(h, w, 4)[..., :3]
        clean = np.fromfile(p + "_clean.f32", np.float32).reshape(h, w, 3)
        albedo = np.fromfile(p + "_albedo.u8", np.uint8).reshape(h, w, 4)[..., :3]
        mse = ((noisy[::4, ::4].astype(np.float32) - clean[::4, ::4]) ** 2).mean()
        if mse < 1e-4:
            continue
        # CHW tensors, kept compact on the CPU: f16 light, u8 albedo
        samples.append((torch.from_numpy(np.ascontiguousarray(noisy.transpose(2, 0, 1))),
                        torch.from_numpy(np.ascontiguousarray(albedo.transpose(2, 0, 1))),
                        torch.from_numpy(clean.astype(np.float16).transpose(2, 0, 1).copy())))
    return samples, n


def to_input(noisy, albedo):
    return torch.cat([noisy.float(), albedo.float() / 255.0], dim=1)


def random_batch(samples, bs, crop, device):
    xs, ys = [], []
    for _ in range(bs):
        noisy, albedo, clean = samples[np.random.randint(len(samples))]
        _, h, w = noisy.shape
        y0, x0 = np.random.randint(h - crop), np.random.randint(w - crop)
        sl = (slice(None), slice(y0, y0 + crop), slice(x0, x0 + crop))
        xs.append(to_input(noisy[sl][None], albedo[sl][None]))
        ys.append(clean[sl][None].float())
    x, y = torch.cat(xs).to(device), torch.cat(ys).to(device)
    if np.random.rand() < 0.5:
        x, y = x.flip(3), y.flip(3)
    return x, y


def psnr(a, b):
    return 10 * torch.log10(1.0 / F.mse_loss(a, b)).item()


@torch.no_grad()
def evaluate(model, box, samples, device):
    rows = []
    for noisy, albedo, clean in samples:
        x = to_input(noisy[None], albedo[None]).to(device)
        y = clean[None].float().to(device)
        rows.append((psnr(x[:, :3], y), psnr(box(x), y), psnr(model(x), y)))
    return np.array(rows).mean(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="../Run/Data/DenoiseSet")
    ap.add_argument("--out", default="runs/kpcn.pt")
    ap.add_argument("--steps", type=int, default=6000)
    ap.add_argument("--bs", type=int, default=32)
    ap.add_argument("--crop", type=int, default=128)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    device = "cuda"

    t0 = time.time()
    samples, total = load_set(args.data)
    n_val = max(1, len(samples) // 10)
    train, val = samples[:-n_val], samples[-n_val:]
    print(f"{total} captured, {len(samples)} kept, {len(train)} train / {len(val)} val, "
          f"loaded in {time.time() - t0:.0f}s")

    model = KPCN().to(device)
    box = KPCN().to(device).eval()  # zero-init last layer = 5x5 box filter
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.steps)

    best = -1.0
    t0 = time.time()
    for step in range(1, args.steps + 1):
        model.train()
        x, y = random_batch(train, args.bs, args.crop, device)
        loss = F.l1_loss(torch.log1p(model(x).clamp(min=0)), torch.log1p(y))
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()

        if step % 500 == 0 or step == args.steps:
            model.eval()
            p_noisy, p_box, p_model = evaluate(model, box, val, device)
            tag = ""
            if p_model > best:
                best = p_model
                torch.save(model.state_dict(), args.out)
                tag = "  saved"
            print(f"step {step:5d}  loss {loss.item():.4f}  val PSNR noisy {p_noisy:.2f} "
                  f"box {p_box:.2f} model {p_model:.2f}  ({time.time() - t0:.0f}s){tag}", flush=True)

    print(f"best val PSNR {best:.2f} dB -> {args.out}")


if __name__ == "__main__":
    main()
