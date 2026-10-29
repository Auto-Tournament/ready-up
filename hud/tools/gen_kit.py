#!/usr/bin/env python3
"""Writes hud/layout/at_kit.xml: the Auto Tournament HUD, three elements, one shown at a time.

  card  (#at_card)  left, above the chat: routine information
  mid   (#at_mid)   centre of the lower half: moments everyone must see
  end   (#at_end)   the leaderboard when a map or series is over (blurs the world)

The layout is generated because of its fixed slots (sentence parts, command slots, 2 x 5
leaderboard rows); the ids and classes the server drives are in hud/README.md. Run it after
changing the structure:  python hud/tools/gen_kit.py
The full component compiler (P2) replaces this.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "layout" / "at_kit.xml"


def label(id_=None, cls="", text="{s:text}"):
    i = f' id="{id_}"' if id_ else ""
    return f'<Label{i} class="{cls}" text="{text}" hittest="false" />'


def panel(id_=None, cls="", children="", hit=False):
    i = f' id="{id_}"' if id_ else ""
    h = "true" if hit else "false"
    if not children:
        return f'<Panel{i} class="{cls}" hittest="{h}" />'
    return f'<Panel{i} class="{cls}" hittest="{h}">{children}</Panel>'


def brand():
    return panel(cls="at-brand", children=panel(cls="at-brandlogo") + label(cls="at-brandname", text="AUTO TOURNAMENT"))


def sentence(prefix):
    # Five parts: text, bold, text, command, text. Each part can be hidden, bold, a command or coloured.
    return panel(cls="at-sentence", children="".join(
        panel(f"{prefix}{n}", "at-part", label(f"{prefix}{n}_t", "at-parttext")) for n in range(5)))


def card():
    head = panel(cls="at-head", children=(
        panel(cls="at-dot")
        + panel(cls="at-lyrbox at-kindbox", children=label("at_kind_a", "at-lyr at-kind cur") + label("at_kind_b", "at-lyr at-kind old"))
        + panel(cls="at-lyrbox", children=label("at_text_a", "at-lyr at-text cur") + label("at_text_b", "at-lyr at-text old"))))
    rules = panel(cls="at-body at-rules", children="".join(
        panel(f"at_r{n}", "at-rule", label(f"at_r{n}_k", "at-rulek") + label(f"at_r{n}_v", "at-rulev")) for n in range(3)))
    line = panel(cls="at-body at-line-body", children=sentence("at_p"))
    dmg_head = panel(cls="at-drow at-dhead", children=(
        label(cls="at-dname", text="") + label(cls="at-dnum", text="GIVEN") + label(cls="at-dnum", text="TAKEN") + label(cls="at-dhp", text="HP")))
    dmg_rows = "".join(panel(f"at_d{n}", "at-drow", (
        label(f"at_d{n}_name", "at-dname") + label(f"at_d{n}_given", "at-dnum at-given")
        + label(f"at_d{n}_taken", "at-dnum at-taken") + label(f"at_d{n}_hp", "at-dhp"))) for n in range(5))
    dmg = panel(cls="at-body at-dmg", children=dmg_head + dmg_rows)
    bar = panel(cls="at-body at-bar", children=(
        panel(cls="at-segs", children="".join(panel(f"at_s{n}", "at-seg") for n in range(10))) + label("at_pct", "at-pct")))
    chat = label(cls="at-chat", text="Press Y to chat")
    # The divider runs edge to edge, so the brand row and the content carry their own padding.
    flow = panel(cls="at-cardflow", children=(
        panel(cls="at-brandrow", children=brand()) + panel(cls="at-divider at-carddivider")
        + panel(cls="at-content", children=head + rules + line + dmg + bar + chat)))
    return panel("at_card", "at-card", flow + panel(cls="at-line") + panel(cls="at-ring"))


def mid():
    confirm = panel(cls="at-confirm", children="".join(
        panel(f"at_mc{n}", "at-confirmrow", label(f"at_mc{n}_team", "at-confirmteam") + label(f"at_mc{n}_state", "at-confirmstate"))
        for n in range(2)))
    cmds = panel(cls="at-cmds", children="".join(
        panel(f"at_mk{n}", "at-cmd", label(f"at_mk{n}_l", "at-cmdlabel") + panel(cls="at-chip", children=label(f"at_mk{n}_c", "at-chiptext")))
        for n in range(4)))
    content = panel(cls="at-midcontent", children=(
        brand() + label("at_mkind", "at-mkind") + label("at_mtext", "at-mtext") + label("at_msub", "at-msub")
        + confirm + sentence("at_mp") + cmds + label(cls="at-chat", text="Press Y to chat")))
    extras = label("at_mtimer", "at-mtimer") + label("at_mcounter", "at-mcounter") + panel(cls="at-line") + panel(cls="at-sweep")
    return panel(cls="at-midwrap", children=panel("at_mid", "at-mid", content + extras))


# The end screen is a toolkit (2026-10-27: no publish for a new column, colour or tooltip): fixed
# slots the server fills and styles with utility classes (hud/styles/at_kit.css, "end screen
# utilities"). Every id below is driven by plugins/match/readyup/end_screen.cpp.
END_COLS = 12    # table columns per team (header text, tooltip and width all from the server)
END_ROWS = 6     # player rows per team
END_ROUNDS = 30  # round history slots
END_AWARDS = 4


def end():
    def head_cell(side, n):
        tip = panel(f"at_e{side}h_c{n}_tip", "at-tip", label(f"at_e{side}h_c{n}_tk", "at-tipk") + label(f"at_e{side}h_c{n}_tv", "at-tipv"))
        return panel(f"at_e{side}h_c{n}", "at-ehc", label(f"at_e{side}h_c{n}_t", "at-ehct") + tip, hit=True)

    def table(side):
        head = panel(f"at_e{side}h", "at-erow at-ehead", label(f"at_e{side}_name", "at-en") + "".join(head_cell(side, n) for n in range(END_COLS)))
        rows = "".join(panel(f"at_e{side}{r}", "at-erow", (
            panel(cls="at-enbox", children=label(f"at_e{side}{r}_n", "at-en") + label(f"at_e{side}{r}_g", "at-etag"))
            + "".join(label(f"at_e{side}{r}_c{n}", "at-ec") for n in range(END_COLS)))) for r in range(END_ROWS))
        return panel(f"at_et{side}", "at-eteam", head + rows)

    def teambox(side, align):
        return panel(cls=f"at-eteambox at-e{side}box", children=(
            label(f"at_e_t{side}", f"at-eteamname {align}") + label(f"at_e_side{side}", f"at-esides {align}")))

    score = panel(cls="at-escore", children=(
        teambox("a", "at-right") + label("at_e_sa", "at-enum") + label(cls="at-ecolon", text=":")
        + label("at_e_sb", "at-enum") + teambox("b", "at-left")))
    rounds = panel("at_erounds", "at-erounds", "".join(
        panel(f"at_rt{n}", "at-rt", label(f"at_rt{n}_u", "at-rtp") + panel(cls="at-rtmid") + label(f"at_rt{n}_d", "at-rtp"))
        for n in range(END_ROUNDS)))
    awards = panel("at_eawards", "at-eawards", "".join(
        panel(f"at_aw{n}", "at-aw", label(f"at_aw{n}_l", "at-awl") + panel(cls="at-awrow", children=(
            label(f"at_aw{n}_n", "at-awn") + label(f"at_aw{n}_v", "at-awv")))) for n in range(END_AWARDS)))
    foot = panel(cls="at-efoot", children=label("at_e_where", "at-ewhere") + label("at_e_next", "at-enext"))
    flow = panel(cls="at-ecardflow", children=(
        brand() + panel(cls="at-divider")
        + label("at_e_head", "at-ehead-text") + panel("at_e_howbox", "at-howbox", label("at_e_how", "at-how"))
        + score + rounds + awards + panel(cls="at-eteams", children=table("a") + table("b")) + foot))
    card_ = panel("at_ecard", "at-ecard", flow + panel(cls="at-line"))
    return panel("at_end", "at-end", panel(cls="at-eblur") + card_)


def main():
    body = card() + mid() + end()
    xml = f"""<!--
  Auto Tournament HUD (generated by hud/tools/gen_kit.py: edit that, then run it).
  Design: https://claude.ai/artifact/Nnt5WzmHAQbt8bXpoYKv97
  m_strLayout = "panorama/layout/custom_game/at_kit.xml"
  Only Panel and Label; hittest="false" everywhere; the root has no id.
-->
<root>
	<styles>
		<include src="s2r://panorama/styles/custom_game/at_kit.vcss_c" />
	</styles>
	<Panel class="at-root" hittest="false">{body}</Panel>
</root>
"""
    OUT.write_text(xml.replace("><", ">\n<"), encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
