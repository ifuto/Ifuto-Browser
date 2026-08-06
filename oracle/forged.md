# T1

Para with **b** and *i* and `c` and [L](x) and <span class='z'>html</span> inline end.

## T2 ~~del~~ ***bi*** [link2](https://e.jp/あ) ![alt9](img.png)

### Q1

> quote L1 quote **L2** with [ql](u)
>
>     indented?
>
> - q item1
> - q item2
>   - q nested

| 項目 | 値 |
| ---- | --: |
| a    | -3x |
| 長いセル内容 | -1x |
| \|escaped | x |

not-a-table |
| --- |

```rust
fn main() { println!("hi"); }
```

```
plain fence
```

~~~

tilde fence

~~~

1. one
2. two
3. three

- a
- b
  - b1
  - b2
- c

- loose a

- loose b

+ plus1
+ plus2

---

***

___

#### Foot

Use note[^a] and another[^b].

##### Misc

Escaped \*not bold\* and \`not code\`.

日本語の段落テキスト。全角カナ、漢字、ひらがな混在 ABC123。

Mixed 日本語 and English in one line with <b>inline html</b> here.

<div>html block</div>

<a name="anchor"></a>

Nested <a href="x">outer <a href="y">inner</a></a> taint case.

###### Deepest H6

文字だけの行
  インデント2の行
	tab leader

半角カナ ABC [link with **bold label**](dest)

*em with `code` inside*

**bold with *em* inside**

End with trailing ws  

[^a]: First note
[^b]: Second with **bold**
