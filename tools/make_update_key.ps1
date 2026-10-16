# 生成更新清单验签用的 RSA-2048 钥匙对。
#
# 输出两样东西：
#   1) 公钥（BCrypt RSAPUBLICBLOB 的 base64）——抄进 src/core/UpdateKey.h；
#   2) 私钥的 n / d（十六进制）——只给"给测试清单签名"的回归脚本用。
#
# 正式发布时：在**离线**机器上运行本脚本，公钥进 UpdateKey.h，私钥单独保管，
# 绝不可把正式私钥提交进仓库（仓库里那把是一次性开发钥匙）。
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$parameters = $rsa.ExportParameters($true)
function ConvertTo-Hex([byte[]]$bytes) { ($bytes | ForEach-Object { $_.ToString('x2') }) -join '' }
$blob = New-Object System.Collections.Generic.List[byte]
$blob.AddRange([BitConverter]::GetBytes([uint32]0x31415352))   # 'RSA1'
$blob.AddRange([BitConverter]::GetBytes([uint32]2048))         # bitLength
$blob.AddRange([BitConverter]::GetBytes([uint32]$parameters.Exponent.Length))
$blob.AddRange([BitConverter]::GetBytes([uint32]$parameters.Modulus.Length))
$blob.AddRange([BitConverter]::GetBytes([uint32]0))            # cbPrime1
$blob.AddRange([BitConverter]::GetBytes([uint32]0))            # cbPrime2
$blob.AddRange($parameters.Exponent)
$blob.AddRange($parameters.Modulus)
Write-Output ("public key (base64, RSAPUBLICBLOB): " + [Convert]::ToBase64String($blob.ToArray()))
Write-Output ("n = " + (ConvertTo-Hex $parameters.Modulus))
Write-Output ("d = " + (ConvertTo-Hex $parameters.D))
