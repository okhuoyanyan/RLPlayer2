# RLPlayer 反馈 Worker · 部署指引（Windows 主线）

> 面向：第一次用 Cloudflare 的人。照着点，不用理解每一步在干什么。
> 总耗时约 **20 分钟**（含注册）。做完你会得到一个网址，填进代码就能用。

---

## 你要准备什么

| 项 | 说明 |
|---|---|
| 一个邮箱 | 注册 Cloudflare 用 |
| GitHub 账号 | 你已经有了（okhuoyanyan） |
| 20 分钟 | 中途不要断，wrangler 登录会开浏览器 |

---

## 第 1 步：注册 Cloudflare（约 5 分钟）

1. 打开 https://dash.cloudflare.com/sign-up
2. 填邮箱 + 密码 → 点 **Sign Up**
3. 去邮箱点验证链接
4. 登录后如果问你"要不要添加网站"，**直接跳过**（Skip / Cancel）——我们只用它的 Worker 和 R2，不托管域名

**完成标志**：能看到 Cloudflare 的仪表盘（Dashboard）。

---

## 第 2 步：建 R2 存储桶（约 3 分钟）

R2 是存用户诊断包的地方（免费 10 GB）。

1. 左侧菜单找 **R2 Object Storage**（如果看不见，点 "All products" 展开）
2. 点 **Create bucket**
3. **Bucket name** 填：`rlplayer-feedback`
4. 其它全部默认 → 点 **Create bucket**

⚠️ **必须先点开通**：如果 R2 页面提示要绑定支付方式才能开通——
   R2 有免费额度，但 Cloudflare 要求先绑卡才能激活。**绑卡不会扣钱**（只要你没超 10 GB）。
   不愿意绑卡也行：跳过这一步，Worker 会在 issue 里写「诊断包未送达」，功能仍可用，只是没有附件。

**完成标志**：R2 页面里能看到一个叫 `rlplayer-feedback` 的桶。

---

## 第 3 步：建 GitHub 反馈仓库（约 2 分钟）

1. 打开 https://github.com/new
2. **Repository name**：`RLPlayer-feedback`
3. 选 **Public**（公开）——别人能搜到已有人报过的问题
4. **不要**勾 "Add a README"（保持空仓库）
5. 点 **Create repository**

**完成标志**：能看到 `okhuoyanyan/RLPlayer-feedback` 这个仓库。

---

## 第 4 步：建 GitHub Token（约 3 分钟，**这步最关键**）

1. 打开 https://github.com/settings/personal-access-tokens/new
2. **Token name**：`rlplayer-feedback-worker`
3. **Expiration**：选 `No expiration`（否则一年后反馈会突然失效）
4. **Repository access**：选 **Only select repositories** → 只勾 `RLPlayer-feedback`
5. **Permissions** 展开 → 找到 **Issues** → 改成 **Read and write**
6. **其它权限全部保持 No access**（尤其是 Contents、Actions，一个都不要给）
7. 点 **Generate token**
8. **立刻复制**那串 `github_pat_...`（关掉页面就再也看不到了）

⚠️ 这个 token 只能动 `RLPlayer-feedback` 这一个仓库的 issue，**不能读你的代码**。
   即使泄露，最坏结果是别人往你的 issue 里灌垃圾——随时可以在这里点 Revoke 作废。

**完成标志**：剪贴板里有一串 `github_pat_` 开头的字符。

---

## 第 5 步：安装 wrangler 并部署（约 5 分钟）

打开 **PowerShell**，逐行复制粘贴（每行回车等它跑完）：

```powershell
# 1) 安装 wrangler（Cloudflare 的命令行工具）
npm install -g wrangler

# 2) 登录（会弹出浏览器，点 Allow）
wrangler login

# 3) 进到 worker 目录
cd D:\RTA-hqplayer\worker

# 4) 填 token（粘贴第 4 步那串 github_pat_...，回车）
wrangler secret put GITHUB_TOKEN

# 5) 填限流盐（随便打一串乱码就行，比如 x7Kp2mQ9vL）
wrangler secret put SHARED_SALT

# 6) 部署
wrangler deploy
```

**如果没有 npm**：先装 Node.js（https://nodejs.org 选 LTS，一路下一步），再回来跑第 1 条。

**部署成功的标志**：终端最后打印类似
```
Published rlplayer-feedback (0.42 sec)
  https://rlplayer-feedback.你的子域.workers.dev
```

**把那行网址复制下来**，下一步要用。

---

## 第 6 步：验证（1 分钟）

在 PowerShell 里跑（把网址换成你刚才拿到的）：

```powershell
$w = 'https://rlplayer-feedback.你的子域.workers.dev'
Invoke-RestMethod -Uri $w -Method Post -ContentType 'application/json' -Body '{"text":"部署测试，可忽略","version":"test"}'
```

**期望输出**：`ok : True` + `id : FB-20261027-XXXXX` + `issue : 1`

然后去 https://github.com/okhuoyanyan/RLPlayer-feedback/issues 看一眼，**应该有一条新 issue**。

看到 issue = **全部成功**。把那个网址发我，我填进代码。

---

## 出错怎么办

| 错误 | 原因 | 解决 |
|---|---|---|
| `wrangler: 无法识别的命令` | 没装 Node.js 或 npm 路径没生效 | 重装 Node.js LTS，重开 PowerShell |
| `Authentication error` | wrangler 没登录 | 重跑 `wrangler login` |
| `R2 bucket not found` | 第 2 步没做或名字打错 | 回第 2 步，桶名必须是 `rlplayer-feedback` |
| 返回 `Worker 未配置` | secret 没填成功 | 重跑第 4、5 条，`wrangler secret list` 应该看到两条 |
| 返回 403 | token 权限不对 | 重做第 4 步：Issues 必须是 Read and write |
| 返回 429 | 限流 | 等 1 分钟再试（同一 IP 每分钟 5 条） |
| issue 建了但没附件 | R2 没开通 | 正常降级；要附件就回第 2 步开通 R2 |

---

## 你做完之后我做什么

1. 把你给的 Worker 网址填进 `src/net/feedback.h` 的 `kWorkerUrlDefault`
2. 把 `feedback.h` 的上传路径从 `PUT /repos/.../contents/`（写 Git）改成 `POST <worker>`
3. 用真实客户端跑一次端到端：点反馈 → 看到 issue 出现 → 附件在 R2
4. 把这个流程写进《使用说明书》的"反馈问题"一节

---

## 附：为什么不能把 token 直接放进软件

软件是公开开源的。一旦 token 进了 exe，任何人 `strings RLPlayer.exe` 就能捞出来，
然后拿你的账号往仓库里灌东西。所以必须有中间人——这就是 Worker 存在的全部理由。
