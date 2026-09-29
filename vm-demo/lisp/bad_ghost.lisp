; 负例：extern ghost 声明了但任何单元都没有 def —— 编译过、链接期报
; undefined global 'ghost'（extern = 承诺链接环境补项，编译器不当 builtin）
(extern ghost)
(ghost 1)