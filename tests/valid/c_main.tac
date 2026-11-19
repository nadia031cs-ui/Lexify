    x = 10
    y = 0
L1:
    t1 = x > 0
    ifFalse t1 goto L2
    t2 = y + x
    y = t2
    t3 = x - 1
    x = t3
    goto L1
L2:
    print y
